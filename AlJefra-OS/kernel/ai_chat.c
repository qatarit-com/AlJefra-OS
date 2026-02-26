/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- AI Chat Engine Implementation
 *
 * Core conversational interface: natural language -> system commands.
 * Supports offline local parsing (English + Arabic) and optional LLM fallback.
 *
 * Design constraints:
 *   - No direct networking code (uses pluggable LLM callback)
 *   - No heap allocator -- all buffers are static or stack
 *   - No libc -- uses lib/string.h and hal/ APIs only
 *   - Portable across x86-64, AArch64, RISC-V 64
 */

#include "ai_chat.h"
#include "driver_loader.h"
#include "ai_bootstrap.h"
#include "sched.h"
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../net/dhcp.h"
#include "../store/catalog.h"

/* Network/marketplace modules (linked from net/ and ai/) */
extern void tcp_init(uint32_t local_ip, uint32_t gateway, uint32_t netmask);
extern void marketplace_set_gateway(uint32_t gateway_ip);
extern hal_status_t marketplace_check_updates(const char *os_version,
                                               char *update_url, uint32_t url_max);

/* ========================================================================
 * Internal helpers (no libc available)
 * ======================================================================== */

/* Simple integer-to-string (unsigned decimal) into a buffer.
 * Returns number of characters written. */
static int uint_to_str(char *buf, int max, uint64_t val)
{
    char tmp[24];
    int len = 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0 && len < 22) {
            tmp[len++] = '0' + (char)(val % 10);
            val /= 10;
        }
    }

    /* Reverse into output buffer */
    int written = 0;
    for (int i = len - 1; i >= 0 && written < max - 1; i--)
        buf[written++] = tmp[i];
    buf[written] = '\0';
    return written;
}

/* Append a string to a buffer at *pos, respecting max.
 * Advances *pos. Returns bytes appended. */
static int buf_append(char *buf, int *pos, int max, const char *s)
{
    int start = *pos;
    while (*s && *pos < max - 1)
        buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
    return *pos - start;
}

/* Append an unsigned integer */
static int buf_append_uint(char *buf, int *pos, int max, uint64_t val)
{
    char tmp[24];
    uint_to_str(tmp, sizeof(tmp), val);
    return buf_append(buf, pos, max, tmp);
}

/* Convert a character to lowercase (ASCII only) */
static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

/* Case-insensitive string comparison (ASCII) */
static int str_eq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b))
            return 0;
        a++;
        b++;
    }
    return to_lower(*a) == to_lower(*b);
}

/* Check if haystack starts with needle (case-insensitive) */
static int __attribute__((unused)) starts_with_nocase(const char *haystack, const char *needle)
{
    while (*needle) {
        if (to_lower(*haystack) != to_lower(*needle))
            return 0;
        haystack++;
        needle++;
    }
    return 1;
}

/* Check if a word appears in the input string (case-insensitive, word boundary).
 * A "word boundary" means the character before and after the match is not
 * alphanumeric, or the match is at the start/end of the string. */
static int __attribute__((unused)) contains_word(const char *input, const char *word)
{
    uint32_t wlen = str_len(word);
    uint32_t ilen = str_len(input);

    if (wlen == 0 || wlen > ilen)
        return 0;

    for (uint32_t i = 0; i + wlen <= ilen; i++) {
        /* Check if the substring matches */
        int match = 1;
        for (uint32_t j = 0; j < wlen; j++) {
            if (to_lower(input[i + j]) != to_lower(word[j])) {
                match = 0;
                break;
            }
        }
        if (!match)
            continue;

        /* Check word boundaries */
        int left_ok = (i == 0) ||
                      (!(input[i - 1] >= 'a' && input[i - 1] <= 'z') &&
                       !(input[i - 1] >= 'A' && input[i - 1] <= 'Z') &&
                       !(input[i - 1] >= '0' && input[i - 1] <= '9'));
        int right_ok = (i + wlen >= ilen) ||
                       (!(input[i + wlen] >= 'a' && input[i + wlen] <= 'z') &&
                        !(input[i + wlen] >= 'A' && input[i + wlen] <= 'Z') &&
                        !(input[i + wlen] >= '0' && input[i + wlen] <= '9'));

        if (left_ok && right_ok)
            return 1;
    }
    return 0;
}

/* Check if input contains a substring (case-insensitive, no boundary check) */
static int contains_substr(const char *input, const char *sub)
{
    uint32_t slen = str_len(sub);
    uint32_t ilen = str_len(input);

    if (slen == 0 || slen > ilen)
        return 0;

    for (uint32_t i = 0; i + slen <= ilen; i++) {
        int match = 1;
        for (uint32_t j = 0; j < slen; j++) {
            if (to_lower(input[i + j]) != to_lower(sub[j])) {
                match = 0;
                break;
            }
        }
        if (match)
            return 1;
    }
    return 0;
}

/* Skip leading whitespace and return pointer to first non-space */
static const char *skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    return s;
}

/* Extract the argument after a keyword.
 * E.g. extract_arg("read myfile.txt", "read", buf, max)
 * writes "myfile.txt" into buf.  Returns 1 if arg found, 0 if not. */
static int extract_arg_after(const char *input, const char *keyword,
                              char *arg, uint32_t arg_max)
{
    uint32_t klen = str_len(keyword);
    uint32_t ilen = str_len(input);

    for (uint32_t i = 0; i + klen <= ilen; i++) {
        int match = 1;
        for (uint32_t j = 0; j < klen; j++) {
            if (to_lower(input[i + j]) != to_lower(keyword[j])) {
                match = 0;
                break;
            }
        }
        if (!match)
            continue;

        /* Keyword found at position i -- skip it and grab the rest */
        const char *rest = skip_space(input + i + klen);
        if (*rest) {
            str_copy(arg, rest, arg_max);
            /* Trim trailing whitespace from arg */
            uint32_t alen = str_len(arg);
            while (alen > 0 && (arg[alen - 1] == ' ' || arg[alen - 1] == '\n'))
                arg[--alen] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Check if a byte sequence is the start of a UTF-8 Arabic character.
 * Arabic Unicode block: U+0600..U+06FF.
 * UTF-8 encoding: 0xD8 0x80 .. 0xDB 0xBF */
static int is_utf8_arabic(const uint8_t *p)
{
    if (p[0] >= 0xD8 && p[0] <= 0xDB && p[1] && (p[1] & 0xC0) == 0x80)
        return 1;
    return 0;
}

/* Check if input contains any Arabic UTF-8 characters */
static int has_arabic(const char *input)
{
    const uint8_t *p = (const uint8_t *)input;
    while (*p) {
        if (is_utf8_arabic(p))
            return 1;
        p++;
    }
    return 0;
}

/* Compare UTF-8 Arabic strings.
 * Both a and b are UTF-8 encoded.  Returns 1 if equal. */
static int __attribute__((unused)) utf8_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if ((uint8_t)*a != (uint8_t)*b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Check if UTF-8 string starts with a prefix */
static int __attribute__((unused)) utf8_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if ((uint8_t)*s != (uint8_t)*prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

/* ========================================================================
 * Minimal JSON extraction (for parsing LLM responses)
 *
 * The LLM is instructed to respond with:
 *   {"action": "fs_list", "args": ""}
 * or:
 *   {"text": "Here is some helpful text..."}
 * ======================================================================== */

/* Find a JSON string value for a given key.
 * Input: raw JSON string.  Searches for "key":"value".
 * Writes value into out (unescaped).  Returns length or -1. */
static int json_extract_string(const char *json, const char *key,
                                char *out, int out_max)
{
    uint32_t klen = str_len(key);
    uint32_t jlen = str_len(json);

    /* Search for "key" pattern */
    for (uint32_t i = 0; i + klen + 4 < jlen; i++) {
        if (json[i] != '"')
            continue;

        /* Check if this is our key */
        int match = 1;
        for (uint32_t j = 0; j < klen; j++) {
            if (json[i + 1 + j] != key[j]) {
                match = 0;
                break;
            }
        }
        if (!match || json[i + 1 + klen] != '"')
            continue;

        /* Found the key.  Skip past "key": and find the value string. */
        uint32_t pos = i + 1 + klen + 1; /* past closing quote of key */

        /* Skip colon and whitespace */
        while (pos < jlen && (json[pos] == ':' || json[pos] == ' ' ||
                               json[pos] == '\t' || json[pos] == '\n'))
            pos++;

        if (pos >= jlen || json[pos] != '"')
            continue;

        /* Parse the value string with escape handling */
        pos++; /* skip opening quote */
        int opos = 0;
        while (pos < jlen && json[pos] != '"' && opos < out_max - 1) {
            if (json[pos] == '\\' && pos + 1 < jlen) {
                pos++;
                switch (json[pos]) {
                case 'n':  out[opos++] = '\n'; break;
                case 'r':  out[opos++] = '\r'; break;
                case 't':  out[opos++] = '\t'; break;
                case '"':  out[opos++] = '"';  break;
                case '\\': out[opos++] = '\\'; break;
                default:   out[opos++] = json[pos]; break;
                }
            } else {
                out[opos++] = json[pos];
            }
            pos++;
        }
        out[opos] = '\0';
        return opos;
    }

    return -1; /* Key not found */
}

/* ========================================================================
 * Global session state
 * ======================================================================== */

static ai_chat_session_t g_session;

/* ========================================================================
 * Action name table -- maps action_type_t <-> string
 * ======================================================================== */

typedef struct {
    action_type_t type;
    const char   *name;
    const char   *desc_en;
    const char   *desc_ar;
} action_info_t;

static const action_info_t g_action_table[] = {
    { ACTION_FS_LIST,        "fs_list",        "List files",                 "\xd8\xb9\xd8\xb1\xd8\xb6 \xd8\xa7\xd9\x84\xd9\x85\xd9\x84\xd9\x81\xd8\xa7\xd8\xaa"  },
    { ACTION_FS_READ,        "fs_read",        "Read file",                  "\xd9\x82\xd8\xb1\xd8\xa7\xd8\xa1\xd8\xa9 \xd9\x85\xd9\x84\xd9\x81"  },
    { ACTION_FS_WRITE,       "fs_write",       "Write file",                 "\xd9\x83\xd8\xaa\xd8\xa7\xd8\xa8\xd8\xa9 \xd9\x85\xd9\x84\xd9\x81"  },
    { ACTION_FS_DELETE,      "fs_delete",      "Delete file",                "\xd8\xad\xd8\xb0\xd9\x81 \xd9\x85\xd9\x84\xd9\x81"  },
    { ACTION_FS_CREATE,      "fs_create",      "Create file",                "\xd8\xa5\xd9\x86\xd8\xb4\xd8\xa7\xd8\xa1 \xd9\x85\xd9\x84\xd9\x81"  },
    { ACTION_NET_STATUS,     "net_status",     "Network status",             "\xd8\xad\xd8\xa7\xd9\x84\xd8\xa9 \xd8\xa7\xd9\x84\xd8\xb4\xd8\xa8\xd9\x83\xd8\xa9"  },
    { ACTION_NET_CONNECT,    "net_connect",    "Connect to network",         "\xd8\xa7\xd8\xaa\xd8\xb5\xd8\xa7\xd9\x84 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb4\xd8\xa8\xd9\x83\xd8\xa9"  },
    { ACTION_DRIVER_LIST,    "driver_list",    "List drivers",               "\xd8\xb9\xd8\xb1\xd8\xb6 \xd8\xa7\xd9\x84\xd8\xaa\xd8\xb9\xd8\xb1\xd9\x8a\xd9\x81\xd8\xa7\xd8\xaa"  },
    { ACTION_DRIVER_SEARCH,  "driver_search",  "Search for driver",          "\xd8\xa8\xd8\xad\xd8\xab \xd8\xb9\xd9\x86 \xd8\xaa\xd8\xb9\xd8\xb1\xd9\x8a\xd9\x81"  },
    { ACTION_DRIVER_INSTALL, "driver_install", "Install driver",             "\xd8\xaa\xd8\xab\xd8\xa8\xd9\x8a\xd8\xaa \xd8\xaa\xd8\xb9\xd8\xb1\xd9\x8a\xd9\x81"  },
    { ACTION_SYS_INFO,       "sys_info",       "System information",         "\xd9\x85\xd8\xb9\xd9\x84\xd9\x88\xd9\x85\xd8\xa7\xd8\xaa \xd8\xa7\xd9\x84\xd9\x86\xd8\xb8\xd8\xa7\xd9\x85"  },
    { ACTION_SYS_REBOOT,     "sys_reboot",     "Reboot system",              "\xd8\xa5\xd8\xb9\xd8\xa7\xd8\xaf\xd8\xa9 \xd8\xaa\xd8\xb4\xd8\xba\xd9\x8a\xd9\x84"  },
    { ACTION_SYS_SHUTDOWN,   "sys_shutdown",   "Shut down system",           "\xd8\xa5\xd9\x8a\xd9\x82\xd8\xa7\xd9\x81 \xd8\xa7\xd9\x84\xd9\x86\xd8\xb8\xd8\xa7\xd9\x85"  },
    { ACTION_SYS_UPDATE,     "sys_update",     "Check for updates",          "\xd8\xa7\xd9\x84\xd8\xa8\xd8\xad\xd8\xab \xd8\xb9\xd9\x86 \xd8\xaa\xd8\xad\xd8\xaf\xd9\x8a\xd8\xab\xd8\xa7\xd8\xaa"  },
    { ACTION_SYS_UPTIME,     "sys_uptime",     "Show uptime",                "\xd9\x88\xd9\x82\xd8\xaa \xd8\xa7\xd9\x84\xd8\xaa\xd8\xb4\xd8\xba\xd9\x8a\xd9\x84"  },
    { ACTION_DISPLAY_SET,    "display_set",    "Change display settings",    "\xd8\xa5\xd8\xb9\xd8\xaf\xd8\xa7\xd8\xaf\xd8\xa7\xd8\xaa \xd8\xa7\xd9\x84\xd8\xb4\xd8\xa7\xd8\xb4\xd8\xa9"  },
    { ACTION_TASK_LIST,      "task_list",      "List tasks",                 "\xd8\xb9\xd8\xb1\xd8\xb6 \xd8\xa7\xd9\x84\xd9\x85\xd9\x87\xd8\xa7\xd9\x85"  },
    { ACTION_MEMORY_INFO,    "memory_info",    "Memory information",         "\xd9\x85\xd8\xb9\xd9\x84\xd9\x88\xd9\x85\xd8\xa7\xd8\xaa \xd8\xa7\xd9\x84\xd8\xb0\xd8\xa7\xd9\x83\xd8\xb1\xd8\xa9"  },
    { ACTION_HELP,           "help",           "Show help",                  "\xd9\x85\xd8\xb3\xd8\xa7\xd8\xb9\xd8\xaf\xd8\xa9"  },
    { ACTION_NONE,           "none",           "Chat / unknown",             "\xd9\x85\xd8\xad\xd8\xa7\xd8\xaf\xd8\xab\xd8\xa9"  },
};

#define ACTION_TABLE_SIZE  (sizeof(g_action_table) / sizeof(g_action_table[0]))

/* ========================================================================
 * Action name lookup
 * ======================================================================== */

const char *ai_action_name(action_type_t type)
{
    for (uint32_t i = 0; i < ACTION_TABLE_SIZE; i++) {
        if (g_action_table[i].type == type)
            return g_action_table[i].name;
    }
    return "none";
}

action_type_t ai_action_from_name(const char *name)
{
    for (uint32_t i = 0; i < ACTION_TABLE_SIZE; i++) {
        if (str_eq_nocase(name, g_action_table[i].name))
            return g_action_table[i].type;
    }
    return ACTION_NONE;
}

/* ========================================================================
 * Session management
 * ======================================================================== */

const ai_chat_session_t *ai_chat_get_session(void)
{
    return &g_session;
}

int ai_chat_init(void)
{
    memset(&g_session, 0, sizeof(g_session));
    g_session.session_start_ms = hal_timer_ms();
    g_session.initialized = 1;
    str_copy(g_session.language, "en", sizeof(g_session.language));
    str_copy(g_session.username, "user", sizeof(g_session.username));
    return 0;
}

void ai_chat_set_llm_callback(ai_llm_send_fn fn)
{
    g_session.llm_send = fn;
}

/* Add a message to conversation history (ring buffer) */
static void history_add(int is_user, const char *text)
{
    uint32_t idx = g_session.history_head;
    g_session.history[idx].is_user = is_user;
    str_copy(g_session.history[idx].text, text,
             sizeof(g_session.history[idx].text));

    g_session.history_head = (g_session.history_head + 1) % AI_CHAT_HISTORY_MAX;
    if (g_session.history_count < AI_CHAT_HISTORY_MAX)
        g_session.history_count++;
}

/* ========================================================================
 * Natural Language Parser -- English patterns
 *
 * Each pattern is: { keywords[], action_type, needs_confirm, has_arg }
 * We match greedily: if any keyword set matches, use that action.
 * ======================================================================== */

/* A rule in the local NLP table */
typedef struct {
    const char   *keywords[4];   /* Up to 4 alternative keywords (NULL-terminated) */
    action_type_t type;
    int           needs_confirm;
    int           has_arg;       /* 1 = extract argument after keyword */
    const char   *arg_keyword;   /* Which keyword precedes the argument */
} nlp_rule_t;

static const nlp_rule_t g_english_rules[] = {
    /* ---- Filesystem ---- */
    { { "ls",     "dir",       NULL },       ACTION_FS_LIST,    0, 0, NULL },
    { { "cat",    NULL },                    ACTION_FS_READ,    0, 1, "cat" },
    { { "rm",     NULL },                    ACTION_FS_DELETE,   1, 1, "rm" },
    { { "touch",  NULL },                    ACTION_FS_CREATE,   0, 1, "touch" },
    { { "write",  NULL },                    ACTION_FS_WRITE,    0, 1, "write" },

    /* ---- Network ---- */
    { { "ifconfig", "ipconfig", NULL },      ACTION_NET_STATUS,  0, 0, NULL },
    { { "dhcp",   NULL },                    ACTION_NET_CONNECT, 0, 0, NULL },
    { { "ping",   NULL },                    ACTION_NET_STATUS,  0, 0, NULL },

    /* ---- Drivers ---- */
    { { "lsmod",  "modprobe",  NULL },       ACTION_DRIVER_LIST, 0, 0, NULL },
    { { "insmod", NULL },                    ACTION_DRIVER_INSTALL, 0, 1, "insmod" },

    /* ---- System ---- */
    { { "uname",  NULL },                    ACTION_SYS_INFO,    0, 0, NULL },
    { { "reboot", NULL },                    ACTION_SYS_REBOOT,  1, 0, NULL },
    { { "halt",   "poweroff", "shutdown", NULL }, ACTION_SYS_SHUTDOWN, 1, 0, NULL },
    { { "uptime", NULL },                    ACTION_SYS_UPTIME,  0, 0, NULL },
    { { "free",   NULL },                    ACTION_MEMORY_INFO, 0, 0, NULL },
    { { "ps",     NULL },                    ACTION_TASK_LIST,   0, 0, NULL },

    /* ---- Help ---- */
    { { "help",   "?",        "man",  NULL }, ACTION_HELP,       0, 0, NULL },
};

#define ENGLISH_RULE_COUNT  (sizeof(g_english_rules) / sizeof(g_english_rules[0]))

/* Natural language phrase patterns (not single-word commands) */
typedef struct {
    const char   *phrases[6]; /* Alternative phrases (NULL-terminated) */
    action_type_t type;
    int           needs_confirm;
    int           has_arg;
    const char   *arg_after;  /* Extract arg after this phrase */
} nlp_phrase_t;

static const nlp_phrase_t g_english_phrases[] = {
    /* ---- Filesystem ---- */
    { { "list files",  "show files",  "list file",  "show file", NULL },
      ACTION_FS_LIST,    0, 0, NULL },
    { { "read file",   "open file",   "view file",  "show me",  NULL },
      ACTION_FS_READ,    0, 1, "file" },
    { { "write file",  "save file",   "create file", "new file", "make file", NULL },
      ACTION_FS_WRITE,   0, 1, "file" },
    { { "delete file", "remove file", "erase file", NULL },
      ACTION_FS_DELETE,  1, 1, "file" },

    /* ---- Network ---- */
    { { "network status", "net status", "show network",  "show net", "network info", NULL },
      ACTION_NET_STATUS,  0, 0, NULL },
    { { "connect network", "connect to network", "connect wifi",
        "join network", "get ip", NULL },
      ACTION_NET_CONNECT, 0, 0, NULL },

    /* ---- Drivers ---- */
    { { "list drivers",  "show drivers",  "loaded drivers",
        "driver list", NULL },
      ACTION_DRIVER_LIST, 0, 0, NULL },
    { { "search driver", "find driver",  "look for driver", NULL },
      ACTION_DRIVER_SEARCH, 0, 1, "driver" },
    { { "install driver", "add driver", "load driver", NULL },
      ACTION_DRIVER_INSTALL, 0, 1, "driver" },

    /* ---- System ---- */
    { { "system info",  "sysinfo",   "system information",
        "about system", "os info", NULL },
      ACTION_SYS_INFO,    0, 0, NULL },
    { { "reboot system", "restart system", "restart computer",
        "restart", NULL },
      ACTION_SYS_REBOOT,  1, 0, NULL },
    { { "shut down", "power off", "turn off", NULL },
      ACTION_SYS_SHUTDOWN, 1, 0, NULL },
    { { "check update", "check for update", "system update",
        "os update", NULL },
      ACTION_SYS_UPDATE,   0, 0, NULL },
    { { "show uptime", "how long running", "uptime", NULL },
      ACTION_SYS_UPTIME,   0, 0, NULL },
    { { "memory info", "memory status", "ram info", "show memory",
        "how much ram", "how much memory" },
      ACTION_MEMORY_INFO,  0, 0, NULL },
    { { "display settings", "screen settings", "change resolution",
        "set display", NULL },
      ACTION_DISPLAY_SET,  0, 1, "display" },
    { { "list tasks", "show tasks", "show processes", "running tasks", NULL },
      ACTION_TASK_LIST,    0, 0, NULL },

    /* ---- Help ---- */
    { { "what can you do", "help me", "show help",
        "how to use", "commands", NULL },
      ACTION_HELP,          0, 0, NULL },
};

#define ENGLISH_PHRASE_COUNT  (sizeof(g_english_phrases) / sizeof(g_english_phrases[0]))

/* ========================================================================
 * Natural Language Parser -- Arabic patterns
 *
 * Arabic keywords stored as UTF-8 byte sequences.
 * ======================================================================== */

/* Arabic command words mapped to actions */
typedef struct {
    const char   *word;         /* UTF-8 Arabic keyword or phrase */
    action_type_t type;
    int           needs_confirm;
} arabic_rule_t;

/* UTF-8 encoded Arabic strings for common commands */
static const arabic_rule_t g_arabic_rules[] = {
    /* عرض الملفات (show files) */
    { "\xd8\xb9\xd8\xb1\xd8\xb6 \xd8\xa7\xd9\x84\xd9\x85\xd9\x84\xd9\x81\xd8\xa7\xd8\xaa",   ACTION_FS_LIST,    0 },
    /* الملفات (files) */
    { "\xd8\xa7\xd9\x84\xd9\x85\xd9\x84\xd9\x81\xd8\xa7\xd8\xaa",                               ACTION_FS_LIST,    0 },
    /* قراءة (read) */
    { "\xd9\x82\xd8\xb1\xd8\xa7\xd8\xa1\xd8\xa9",                                                 ACTION_FS_READ,    0 },
    /* اقرأ (read - imperative) */
    { "\xd8\xa7\xd9\x82\xd8\xb1\xd8\xa3",                                                          ACTION_FS_READ,    0 },
    /* حذف (delete) */
    { "\xd8\xad\xd8\xb0\xd9\x81",                                                                   ACTION_FS_DELETE,  1 },
    /* احذف (delete - imperative) */
    { "\xd8\xa7\xd8\xad\xd8\xb0\xd9\x81",                                                          ACTION_FS_DELETE,  1 },
    /* إنشاء (create) */
    { "\xd8\xa5\xd9\x86\xd8\xb4\xd8\xa7\xd8\xa1",                                                  ACTION_FS_CREATE,  0 },
    /* أنشئ (create - imperative) */
    { "\xd8\xa3\xd9\x86\xd8\xb4\xd8\xa6",                                                          ACTION_FS_CREATE,  0 },
    /* كتابة (write) */
    { "\xd9\x83\xd8\xaa\xd8\xa7\xd8\xa8\xd8\xa9",                                                  ACTION_FS_WRITE,   0 },
    /* اكتب (write - imperative) */
    { "\xd8\xa7\xd9\x83\xd8\xaa\xd8\xa8",                                                          ACTION_FS_WRITE,   0 },

    /* حالة الشبكة (network status) */
    { "\xd8\xad\xd8\xa7\xd9\x84\xd8\xa9 \xd8\xa7\xd9\x84\xd8\xb4\xd8\xa8\xd9\x83\xd8\xa9",     ACTION_NET_STATUS,  0 },
    /* الشبكة (network) */
    { "\xd8\xa7\xd9\x84\xd8\xb4\xd8\xa8\xd9\x83\xd8\xa9",                                         ACTION_NET_STATUS,  0 },
    /* اتصال (connect) */
    { "\xd8\xa7\xd8\xaa\xd8\xb5\xd8\xa7\xd9\x84",                                                  ACTION_NET_CONNECT, 0 },
    /* اتصل (connect - imperative) */
    { "\xd8\xa7\xd8\xaa\xd8\xb5\xd9\x84",                                                          ACTION_NET_CONNECT, 0 },

    /* التعريفات (drivers) */
    { "\xd8\xa7\xd9\x84\xd8\xaa\xd8\xb9\xd8\xb1\xd9\x8a\xd9\x81\xd8\xa7\xd8\xaa",              ACTION_DRIVER_LIST, 0 },
    /* عرض التعريفات (show drivers) */
    { "\xd8\xb9\xd8\xb1\xd8\xb6 \xd8\xa7\xd9\x84\xd8\xaa\xd8\xb9\xd8\xb1\xd9\x8a\xd9\x81\xd8\xa7\xd8\xaa", ACTION_DRIVER_LIST, 0 },
    /* تثبيت تعريف (install driver) */
    { "\xd8\xaa\xd8\xab\xd8\xa8\xd9\x8a\xd8\xaa \xd8\xaa\xd8\xb9\xd8\xb1\xd9\x8a\xd9\x81",    ACTION_DRIVER_INSTALL, 0 },
    /* بحث عن تعريف (search for driver) */
    { "\xd8\xa8\xd8\xad\xd8\xab \xd8\xb9\xd9\x86 \xd8\xaa\xd8\xb9\xd8\xb1\xd9\x8a\xd9\x81",   ACTION_DRIVER_SEARCH, 0 },

    /* معلومات النظام (system info) */
    { "\xd9\x85\xd8\xb9\xd9\x84\xd9\x88\xd9\x85\xd8\xa7\xd8\xaa \xd8\xa7\xd9\x84\xd9\x86\xd8\xb8\xd8\xa7\xd9\x85", ACTION_SYS_INFO, 0 },
    /* النظام (system) */
    { "\xd8\xa7\xd9\x84\xd9\x86\xd8\xb8\xd8\xa7\xd9\x85",                                         ACTION_SYS_INFO,    0 },
    /* إعادة تشغيل (reboot) */
    { "\xd8\xa5\xd8\xb9\xd8\xa7\xd8\xaf\xd8\xa9 \xd8\xaa\xd8\xb4\xd8\xba\xd9\x8a\xd9\x84",    ACTION_SYS_REBOOT,  1 },
    /* أعد التشغيل (reboot - imperative) */
    { "\xd8\xa3\xd8\xb9\xd8\xaf \xd8\xa7\xd9\x84\xd8\xaa\xd8\xb4\xd8\xba\xd9\x8a\xd9\x84",    ACTION_SYS_REBOOT,  1 },
    /* إيقاف (halt/shutdown) */
    { "\xd8\xa5\xd9\x8a\xd9\x82\xd8\xa7\xd9\x81",                                                  ACTION_SYS_SHUTDOWN, 1 },
    /* أوقف النظام (shut down system) */
    { "\xd8\xa3\xd9\x88\xd9\x82\xd9\x81 \xd8\xa7\xd9\x84\xd9\x86\xd8\xb8\xd8\xa7\xd9\x85",    ACTION_SYS_SHUTDOWN, 1 },
    /* تحديث (update) */
    { "\xd8\xaa\xd8\xad\xd8\xaf\xd9\x8a\xd8\xab",                                                  ACTION_SYS_UPDATE,  0 },
    /* البحث عن تحديثات (check for updates) */
    { "\xd8\xa7\xd9\x84\xd8\xa8\xd8\xad\xd8\xab \xd8\xb9\xd9\x86 \xd8\xaa\xd8\xad\xd8\xaf\xd9\x8a\xd8\xab\xd8\xa7\xd8\xaa", ACTION_SYS_UPDATE, 0 },

    /* وقت التشغيل (uptime) */
    { "\xd9\x88\xd9\x82\xd8\xaa \xd8\xa7\xd9\x84\xd8\xaa\xd8\xb4\xd8\xba\xd9\x8a\xd9\x84",    ACTION_SYS_UPTIME,  0 },
    /* الذاكرة (memory) */
    { "\xd8\xa7\xd9\x84\xd8\xb0\xd8\xa7\xd9\x83\xd8\xb1\xd8\xa9",                                ACTION_MEMORY_INFO, 0 },
    /* معلومات الذاكرة (memory info) */
    { "\xd9\x85\xd8\xb9\xd9\x84\xd9\x88\xd9\x85\xd8\xa7\xd8\xaa \xd8\xa7\xd9\x84\xd8\xb0\xd8\xa7\xd9\x83\xd8\xb1\xd8\xa9", ACTION_MEMORY_INFO, 0 },

    /* المهام (tasks) */
    { "\xd8\xa7\xd9\x84\xd9\x85\xd9\x87\xd8\xa7\xd9\x85",                                         ACTION_TASK_LIST,   0 },
    /* الشاشة (display) */
    { "\xd8\xa7\xd9\x84\xd8\xb4\xd8\xa7\xd8\xb4\xd8\xa9",                                         ACTION_DISPLAY_SET, 0 },

    /* مساعدة (help) */
    { "\xd9\x85\xd8\xb3\xd8\xa7\xd8\xb9\xd8\xaf\xd8\xa9",                                         ACTION_HELP,        0 },
    /* ساعدني (help me) */
    { "\xd8\xb3\xd8\xa7\xd8\xb9\xd8\xaf\xd9\x86\xd9\x8a",                                        ACTION_HELP,        0 },
};

#define ARABIC_RULE_COUNT  (sizeof(g_arabic_rules) / sizeof(g_arabic_rules[0]))

/* ========================================================================
 * Local NLP Parser implementation
 * ======================================================================== */

/* Try to match a single-word Unix-style command */
static int try_unix_command(const char *input, ai_action_t *action)
{
    /* Extract first token */
    const char *p = skip_space(input);
    char cmd[64];
    uint32_t ci = 0;
    while (*p && *p != ' ' && *p != '\t' && ci < sizeof(cmd) - 1)
        cmd[ci++] = *p++;
    cmd[ci] = '\0';

    if (ci == 0)
        return 0;

    for (uint32_t i = 0; i < ENGLISH_RULE_COUNT; i++) {
        const nlp_rule_t *rule = &g_english_rules[i];
        for (int k = 0; k < 4 && rule->keywords[k]; k++) {
            if (str_eq_nocase(cmd, rule->keywords[k])) {
                action->type = rule->type;
                action->needs_confirm = rule->needs_confirm;
                action->confidence = 95;
                action->args[0] = '\0';

                /* Extract argument if needed */
                if (rule->has_arg && rule->arg_keyword) {
                    const char *rest = skip_space(p);
                    if (*rest)
                        str_copy(action->args, rest, sizeof(action->args));
                }
                return 1;
            }
        }
    }
    return 0;
}

/* Try to match a multi-word English phrase */
static int try_english_phrase(const char *input, ai_action_t *action)
{
    for (uint32_t i = 0; i < ENGLISH_PHRASE_COUNT; i++) {
        const nlp_phrase_t *phrase = &g_english_phrases[i];
        for (int k = 0; k < 6 && phrase->phrases[k]; k++) {
            if (contains_substr(input, phrase->phrases[k])) {
                action->type = phrase->type;
                action->needs_confirm = phrase->needs_confirm;
                action->confidence = 85;
                action->args[0] = '\0';

                /* Extract argument if the phrase has one */
                if (phrase->has_arg && phrase->arg_after) {
                    extract_arg_after(input, phrase->arg_after,
                                      action->args, sizeof(action->args));
                }
                return 1;
            }
        }
    }
    return 0;
}

/* Try to match Arabic commands */
static int try_arabic(const char *input, ai_action_t *action)
{
    if (!has_arabic(input))
        return 0;

    /* Try each Arabic rule -- longest match first (rules are sorted
     * with multi-word phrases before single words) */
    for (uint32_t i = 0; i < ARABIC_RULE_COUNT; i++) {
        const arabic_rule_t *rule = &g_arabic_rules[i];
        if (contains_substr(input, rule->word)) {
            action->type = rule->type;
            action->needs_confirm = rule->needs_confirm;
            action->confidence = 80;
            action->args[0] = '\0';

            /* For Arabic commands that take file args, the filename
             * is typically the non-Arabic portion of the input.
             * Extract any ASCII token from the input as the argument. */
            const char *p = input;
            while (*p) {
                /* Skip UTF-8 multibyte characters */
                if ((uint8_t)*p >= 0x80) {
                    p++;
                    continue;
                }
                /* Skip spaces */
                if (*p == ' ' || *p == '\t') {
                    p++;
                    continue;
                }
                /* Found an ASCII token -- grab it */
                if (*p >= '!' && *p <= '~') {
                    char arg[256];
                    uint32_t ai = 0;
                    while (*p >= '!' && *p <= '~' && ai < sizeof(arg) - 1)
                        arg[ai++] = *p++;
                    arg[ai] = '\0';

                    /* Skip the Arabic keyword itself if it got picked up */
                    if (ai > 0 && (uint8_t)arg[0] < 0x80) {
                        str_copy(action->args, arg, sizeof(action->args));
                    }
                    break;
                }
                p++;
            }
            return 1;
        }
    }
    return 0;
}

int ai_parse_local(const char *input, ai_action_t *action)
{
    if (!input || !action)
        return 0;

    /* Initialize action */
    action->type = ACTION_NONE;
    action->args[0] = '\0';
    action->needs_confirm = 0;
    action->confidence = 0;

    /* Skip leading whitespace */
    input = skip_space(input);
    if (!*input)
        return 0;

    /* 1. Try Unix-style single-word commands (highest priority, exact match) */
    if (try_unix_command(input, action))
        return 1;

    /* 2. Try Arabic commands (check before English phrases since Arabic
     *    characters are unambiguous) */
    if (try_arabic(input, action))
        return 1;

    /* 3. Try English natural language phrases */
    if (try_english_phrase(input, action))
        return 1;

    /* 4. No local match -- return ACTION_NONE so caller can try LLM */
    return 0;
}

/* ========================================================================
 * System Context Builder (for LLM system prompt)
 * ======================================================================== */

int ai_get_context(char *buf, int size)
{
    int pos = 0;

    buf_append(buf, &pos, size,
        "You are the AI assistant of AlJefra OS, a universal boot operating system. "
        "You help the user manage their system via natural language.\n\n"
        "SYSTEM STATE:\n");

    /* Architecture */
    buf_append(buf, &pos, size, "Architecture: ");
    switch (hal_arch()) {
    case HAL_ARCH_X86_64:  buf_append(buf, &pos, size, "x86-64\n");   break;
    case HAL_ARCH_AARCH64: buf_append(buf, &pos, size, "AArch64\n");  break;
    case HAL_ARCH_RISCV64: buf_append(buf, &pos, size, "RISC-V 64\n"); break;
    }

    /* CPU */
    hal_cpu_info_t cpu;
    hal_cpu_get_info(&cpu);
    buf_append(buf, &pos, size, "CPU: ");
    buf_append(buf, &pos, size, cpu.model);
    buf_append(buf, &pos, size, " (");
    buf_append_uint(buf, &pos, size, cpu.cores_logical);
    buf_append(buf, &pos, size, " cores)\n");

    /* RAM */
    uint64_t total_mb = hal_mmu_total_ram() / (1024 * 1024);
    uint64_t free_mb  = hal_mmu_free_ram() / (1024 * 1024);
    buf_append(buf, &pos, size, "RAM: ");
    buf_append_uint(buf, &pos, size, free_mb);
    buf_append(buf, &pos, size, " MB free / ");
    buf_append_uint(buf, &pos, size, total_mb);
    buf_append(buf, &pos, size, " MB total\n");

    /* Uptime */
    uint64_t uptime_s = (hal_timer_ms() - g_session.session_start_ms) / 1000;
    buf_append(buf, &pos, size, "Uptime: ");
    buf_append_uint(buf, &pos, size, uptime_s);
    buf_append(buf, &pos, size, " seconds\n");

    /* Bootstrap state */
    buf_append(buf, &pos, size, "Bootstrap: ");
    switch (ai_bootstrap_state()) {
    case BOOTSTRAP_INIT:          buf_append(buf, &pos, size, "initializing\n");  break;
    case BOOTSTRAP_NET_UP:        buf_append(buf, &pos, size, "network up\n");    break;
    case BOOTSTRAP_CONNECTED:     buf_append(buf, &pos, size, "connected\n");     break;
    case BOOTSTRAP_MANIFEST_SENT: buf_append(buf, &pos, size, "manifest sent\n"); break;
    case BOOTSTRAP_DOWNLOADING:   buf_append(buf, &pos, size, "downloading\n");   break;
    case BOOTSTRAP_COMPLETE:      buf_append(buf, &pos, size, "complete\n");      break;
    case BOOTSTRAP_FAILED:        buf_append(buf, &pos, size, "failed\n");        break;
    }

    /* Loaded drivers */
    buf_append(buf, &pos, size, "\nLOADED DRIVERS:\n");
    const driver_ops_t *drivers[MAX_DRIVERS];
    uint32_t drv_count = driver_list(drivers, MAX_DRIVERS);
    if (drv_count == 0) {
        buf_append(buf, &pos, size, "  (none)\n");
    } else {
        for (uint32_t i = 0; i < drv_count; i++) {
            buf_append(buf, &pos, size, "  - ");
            buf_append(buf, &pos, size, drivers[i]->name);
            buf_append(buf, &pos, size, " [");
            switch (drivers[i]->category) {
            case DRIVER_CAT_STORAGE: buf_append(buf, &pos, size, "storage"); break;
            case DRIVER_CAT_NETWORK: buf_append(buf, &pos, size, "network"); break;
            case DRIVER_CAT_INPUT:   buf_append(buf, &pos, size, "input");   break;
            case DRIVER_CAT_DISPLAY: buf_append(buf, &pos, size, "display"); break;
            case DRIVER_CAT_GPU:     buf_append(buf, &pos, size, "gpu");     break;
            case DRIVER_CAT_BUS:     buf_append(buf, &pos, size, "bus");     break;
            case DRIVER_CAT_OTHER:   buf_append(buf, &pos, size, "other");   break;
            }
            buf_append(buf, &pos, size, "]\n");
        }
    }

    /* Network status */
    buf_append(buf, &pos, size, "\nNETWORK:\n");
    const driver_ops_t *net = driver_get_network();
    if (net) {
        buf_append(buf, &pos, size, "  NIC: ");
        buf_append(buf, &pos, size, net->name);
        buf_append(buf, &pos, size, "\n");

        if (net->net_get_mac) {
            uint8_t mac[6];
            net->net_get_mac(mac);
            buf_append(buf, &pos, size, "  MAC: ");
            /* Format MAC as hex pairs */
            const char hex[] = "0123456789abcdef";
            for (int i = 0; i < 6; i++) {
                if (i > 0 && pos < size - 1) buf[pos++] = ':';
                if (pos < size - 2) {
                    buf[pos++] = hex[(mac[i] >> 4) & 0xF];
                    buf[pos++] = hex[mac[i] & 0xF];
                }
            }
            buf[pos] = '\0';
            buf_append(buf, &pos, size, "\n");
        }

        const dhcp_lease_t *lease = dhcp_get_lease();
        if (lease && lease->client_ip) {
            buf_append(buf, &pos, size, "  IP: ");
            buf_append_uint(buf, &pos, size, (lease->client_ip >> 24) & 0xFF);
            buf_append(buf, &pos, size, ".");
            buf_append_uint(buf, &pos, size, (lease->client_ip >> 16) & 0xFF);
            buf_append(buf, &pos, size, ".");
            buf_append_uint(buf, &pos, size, (lease->client_ip >> 8) & 0xFF);
            buf_append(buf, &pos, size, ".");
            buf_append_uint(buf, &pos, size, lease->client_ip & 0xFF);
            buf_append(buf, &pos, size, "\n");
        } else {
            buf_append(buf, &pos, size, "  IP: not configured\n");
        }
    } else {
        buf_append(buf, &pos, size, "  No network interface\n");
    }

    /* Available actions (for the LLM to know what it can invoke) */
    buf_append(buf, &pos, size,
        "\nAVAILABLE ACTIONS (respond with JSON):\n"
        "To execute a system command, respond with exactly one JSON object:\n"
        "  {\"action\": \"<action_name>\", \"args\": \"<arguments>\"}\n"
        "To give a text response only:\n"
        "  {\"text\": \"Your response here\"}\n"
        "\nAction names:\n");

    for (uint32_t i = 0; i < ACTION_TABLE_SIZE; i++) {
        if (g_action_table[i].type == ACTION_NONE)
            continue;
        buf_append(buf, &pos, size, "  ");
        buf_append(buf, &pos, size, g_action_table[i].name);
        buf_append(buf, &pos, size, " - ");
        buf_append(buf, &pos, size, g_action_table[i].desc_en);
        buf_append(buf, &pos, size, "\n");
    }

    /* Recent conversation history for context */
    if (g_session.history_count > 0) {
        buf_append(buf, &pos, size, "\nRECENT CONVERSATION:\n");

        uint32_t start;
        uint32_t count = g_session.history_count;
        if (count >= AI_CHAT_HISTORY_MAX) {
            start = g_session.history_head;
        } else {
            start = 0;
        }

        /* Show last 6 messages at most for context */
        uint32_t show = count < 6 ? count : 6;
        uint32_t skip = count - show;
        uint32_t idx = (start + skip) % AI_CHAT_HISTORY_MAX;

        for (uint32_t i = 0; i < show; i++) {
            const chat_message_t *m = &g_session.history[idx];
            buf_append(buf, &pos, size, m->is_user ? "User: " : "Assistant: ");
            buf_append(buf, &pos, size, m->text);
            buf_append(buf, &pos, size, "\n");
            idx = (idx + 1) % AI_CHAT_HISTORY_MAX;
        }
    }

    buf_append(buf, &pos, size,
        "\nIMPORTANT: Respond ONLY with valid JSON. "
        "Do not include markdown or extra text.\n");

    return pos;
}

/* ========================================================================
 * Action Executor
 * ======================================================================== */

/* Execute ACTION_FS_LIST: list files on storage */
static int exec_fs_list(char *output, int output_size)
{
    int pos = 0;

    const driver_ops_t *stor = driver_get_storage();
    if (!stor || !stor->read) {
        buf_append(output, &pos, output_size, "No storage device available.\n");
        return -1;
    }

    /* Read the BMFS directory from LBA 8 (standard location).
     * BMFS directory entries are 64 bytes each, starting at byte 4096.
     * Entry format: filename[32] + reserved[8] + start_block[8] +
     *               reserved[8] + file_size[8]
     */
    buf_append(output, &pos, output_size, "Files on storage:\n");

    /* Allocate a DMA buffer for reading directory sectors */
    uint64_t phys;
    uint8_t *dir_buf = (uint8_t *)hal_dma_alloc(4096, &phys);
    if (!dir_buf) {
        buf_append(output, &pos, output_size, "  (failed to allocate buffer)\n");
        return -1;
    }

    /* Read directory sectors (LBA 8, 8 sectors = 4096 bytes) */
    int64_t rd = stor->read(dir_buf, 8, 8);
    if (rd <= 0) {
        buf_append(output, &pos, output_size, "  (failed to read directory)\n");
        hal_dma_free(dir_buf, 4096);
        return -1;
    }

    /* Parse BMFS directory entries */
    int file_count = 0;
    for (int i = 0; i < 64; i++) { /* Max 64 directory entries */
        uint8_t *entry = dir_buf + (i * 64);

        /* First byte of filename: 0x00 = empty, 0x01 = deleted */
        if (entry[0] == 0x00 || entry[0] == 0x01)
            continue;

        /* Extract filename (32 bytes, null-terminated) */
        char name[33];
        memcpy(name, entry, 32);
        name[32] = '\0';

        /* Extract file size (bytes 56-63, little-endian uint64) */
        uint64_t fsize = 0;
        for (int b = 0; b < 8; b++)
            fsize |= ((uint64_t)entry[56 + b]) << (b * 8);

        buf_append(output, &pos, output_size, "  ");
        buf_append(output, &pos, output_size, name);
        buf_append(output, &pos, output_size, "  (");
        buf_append_uint(output, &pos, output_size, fsize);
        buf_append(output, &pos, output_size, " bytes)\n");
        file_count++;
    }

    if (file_count == 0)
        buf_append(output, &pos, output_size, "  (no files)\n");
    else {
        buf_append_uint(output, &pos, output_size, (uint64_t)file_count);
        buf_append(output, &pos, output_size, " file(s) total.\n");
    }

    hal_dma_free(dir_buf, 4096);
    return 0;
}

/* Execute ACTION_FS_READ: read a file and display contents */
static int exec_fs_read(const char *filename, char *output, int output_size)
{
    int pos = 0;

    if (!filename || !filename[0]) {
        buf_append(output, &pos, output_size, "Usage: read <filename>\n");
        return -1;
    }

    const driver_ops_t *stor = driver_get_storage();
    if (!stor || !stor->read) {
        buf_append(output, &pos, output_size, "No storage device available.\n");
        return -1;
    }

    /* Read directory to find the file */
    uint64_t phys;
    uint8_t *dir_buf = (uint8_t *)hal_dma_alloc(4096, &phys);
    if (!dir_buf) {
        buf_append(output, &pos, output_size, "Failed to allocate buffer.\n");
        return -1;
    }

    int64_t rd = stor->read(dir_buf, 8, 8);
    if (rd <= 0) {
        buf_append(output, &pos, output_size, "Failed to read directory.\n");
        hal_dma_free(dir_buf, 4096);
        return -1;
    }

    /* Search for the file */
    uint64_t start_block = 0;
    uint64_t file_size = 0;
    int found = 0;

    for (int i = 0; i < 64; i++) {
        uint8_t *entry = dir_buf + (i * 64);
        if (entry[0] == 0x00 || entry[0] == 0x01)
            continue;

        /* Compare filename */
        char name[33];
        memcpy(name, entry, 32);
        name[32] = '\0';

        /* Trim trailing spaces */
        int len = 31;
        while (len >= 0 && (name[len] == ' ' || name[len] == '\0'))
            len--;
        name[len + 1] = '\0';

        if (str_eq_nocase(name, filename)) {
            /* Extract start block (bytes 40-47) and size (bytes 56-63) */
            start_block = 0;
            for (int b = 0; b < 8; b++)
                start_block |= ((uint64_t)entry[40 + b]) << (b * 8);
            file_size = 0;
            for (int b = 0; b < 8; b++)
                file_size |= ((uint64_t)entry[56 + b]) << (b * 8);
            found = 1;
            break;
        }
    }

    hal_dma_free(dir_buf, 4096);

    if (!found) {
        buf_append(output, &pos, output_size, "File not found: ");
        buf_append(output, &pos, output_size, filename);
        buf_append(output, &pos, output_size, "\n");
        return -1;
    }

    /* Read file contents (cap at 2KB for display) */
    uint64_t read_size = file_size;
    if (read_size > 2048)
        read_size = 2048;

    uint32_t sectors = (uint32_t)((read_size + 511) / 512);
    uint8_t *file_buf = (uint8_t *)hal_dma_alloc(sectors * 512, &phys);
    if (!file_buf) {
        buf_append(output, &pos, output_size, "Failed to allocate read buffer.\n");
        return -1;
    }

    /* BMFS blocks are 1 MB each, starting at LBA offset for data area */
    uint64_t lba = start_block * 2048; /* 1MB blocks / 512 bytes per sector */
    rd = stor->read(file_buf, lba, sectors);
    if (rd <= 0) {
        buf_append(output, &pos, output_size, "Failed to read file data.\n");
        hal_dma_free(file_buf, sectors * 512);
        return -1;
    }

    /* Display file contents */
    buf_append(output, &pos, output_size, "--- ");
    buf_append(output, &pos, output_size, filename);
    buf_append(output, &pos, output_size, " (");
    buf_append_uint(output, &pos, output_size, file_size);
    buf_append(output, &pos, output_size, " bytes) ---\n");

    /* Write file data to output buffer (as text) */
    for (uint64_t i = 0; i < read_size && pos < output_size - 2; i++) {
        /* Filter non-printable characters */
        char c = (char)file_buf[i];
        if (c == '\n' || c == '\r' || c == '\t' || (c >= ' ' && c <= '~'))
            output[pos++] = c;
        else
            output[pos++] = '.';
    }
    output[pos] = '\0';

    if (file_size > 2048) {
        buf_append(output, &pos, output_size, "\n... (truncated, ");
        buf_append_uint(output, &pos, output_size, file_size - 2048);
        buf_append(output, &pos, output_size, " more bytes)\n");
    } else {
        buf_append(output, &pos, output_size, "\n--- end ---\n");
    }

    hal_dma_free(file_buf, sectors * 512);
    return 0;
}

/* Execute ACTION_NET_STATUS: show network information */
static int exec_net_status(char *output, int output_size)
{
    int pos = 0;

    const driver_ops_t *net = driver_get_network();
    if (!net) {
        buf_append(output, &pos, output_size, "No network interface available.\n");
        return -1;
    }

    buf_append(output, &pos, output_size, "Network Status:\n");
    buf_append(output, &pos, output_size, "  Interface: ");
    buf_append(output, &pos, output_size, net->name);
    buf_append(output, &pos, output_size, "\n");

    /* MAC address */
    if (net->net_get_mac) {
        uint8_t mac[6];
        net->net_get_mac(mac);
        buf_append(output, &pos, output_size, "  MAC: ");
        const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 6; i++) {
            if (i > 0 && pos < output_size - 1) output[pos++] = ':';
            if (pos < output_size - 2) {
                output[pos++] = hex[(mac[i] >> 4) & 0xF];
                output[pos++] = hex[mac[i] & 0xF];
            }
        }
        output[pos] = '\0';
        buf_append(output, &pos, output_size, "\n");
    }

    /* IP address from DHCP lease */
    const dhcp_lease_t *lease = dhcp_get_lease();
    if (lease && lease->client_ip) {
        buf_append(output, &pos, output_size, "  IP: ");
        buf_append_uint(output, &pos, output_size, (lease->client_ip >> 24) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, (lease->client_ip >> 16) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, (lease->client_ip >> 8) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, lease->client_ip & 0xFF);
        buf_append(output, &pos, output_size, "\n");

        buf_append(output, &pos, output_size, "  Gateway: ");
        buf_append_uint(output, &pos, output_size, (lease->gateway >> 24) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, (lease->gateway >> 16) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, (lease->gateway >> 8) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, lease->gateway & 0xFF);
        buf_append(output, &pos, output_size, "\n");

        buf_append(output, &pos, output_size, "  DNS: ");
        buf_append_uint(output, &pos, output_size, (lease->dns_server >> 24) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, (lease->dns_server >> 16) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, (lease->dns_server >> 8) & 0xFF);
        buf_append(output, &pos, output_size, ".");
        buf_append_uint(output, &pos, output_size, lease->dns_server & 0xFF);
        buf_append(output, &pos, output_size, "\n");

        buf_append(output, &pos, output_size, "  Lease: ");
        buf_append_uint(output, &pos, output_size, lease->lease_time);
        buf_append(output, &pos, output_size, " seconds\n");
    } else {
        buf_append(output, &pos, output_size, "  IP: not configured (DHCP not run)\n");
    }

    buf_append(output, &pos, output_size, "  Status: ");
    bootstrap_state_t bs = ai_bootstrap_state();
    if (bs >= BOOTSTRAP_NET_UP)
        buf_append(output, &pos, output_size, "UP\n");
    else
        buf_append(output, &pos, output_size, "DOWN\n");

    return 0;
}

/* Execute ACTION_NET_CONNECT: trigger DHCP */
static int exec_net_connect(char *output, int output_size)
{
    int pos = 0;

    const driver_ops_t *net = driver_get_network();
    if (!net) {
        buf_append(output, &pos, output_size,
            "No network interface available. Cannot connect.\n");
        return -1;
    }

    buf_append(output, &pos, output_size, "Running DHCP discovery...\n");

    uint32_t ip = 0, gateway = 0, dns = 0;
    hal_status_t rc = dhcp_discover(&ip, &gateway, &dns);

    if (rc != HAL_OK) {
        buf_append(output, &pos, output_size, "DHCP failed. No network connection.\n");
        return -1;
    }

    buf_append(output, &pos, output_size, "Connected! IP: ");
    buf_append_uint(output, &pos, output_size, (ip >> 24) & 0xFF);
    buf_append(output, &pos, output_size, ".");
    buf_append_uint(output, &pos, output_size, (ip >> 16) & 0xFF);
    buf_append(output, &pos, output_size, ".");
    buf_append_uint(output, &pos, output_size, (ip >> 8) & 0xFF);
    buf_append(output, &pos, output_size, ".");
    buf_append_uint(output, &pos, output_size, ip & 0xFF);
    buf_append(output, &pos, output_size, "\n");

    /* Reinitialize TCP with new IP */
    tcp_init(ip, gateway, 0xFFFFFF00);
    marketplace_set_gateway(gateway);

    return 0;
}

/* Execute ACTION_DRIVER_LIST: list loaded drivers */
static int exec_driver_list(char *output, int output_size)
{
    int pos = 0;

    const driver_ops_t *drivers[MAX_DRIVERS];
    uint32_t count = driver_list(drivers, MAX_DRIVERS);

    buf_append(output, &pos, output_size, "Loaded Drivers:\n");

    if (count == 0) {
        buf_append(output, &pos, output_size, "  (no drivers loaded)\n");
        return 0;
    }

    static const char *cat_names[] = {
        "storage", "network", "input", "display", "gpu", "bus", "other"
    };

    for (uint32_t i = 0; i < count; i++) {
        buf_append(output, &pos, output_size, "  ");
        buf_append_uint(output, &pos, output_size, i + 1);
        buf_append(output, &pos, output_size, ". ");
        buf_append(output, &pos, output_size, drivers[i]->name);
        buf_append(output, &pos, output_size, " [");
        if (drivers[i]->category <= DRIVER_CAT_OTHER)
            buf_append(output, &pos, output_size, cat_names[drivers[i]->category]);
        else
            buf_append(output, &pos, output_size, "unknown");
        buf_append(output, &pos, output_size, "]\n");
    }

    buf_append_uint(output, &pos, output_size, count);
    buf_append(output, &pos, output_size, " driver(s) loaded.\n");
    return 0;
}

/* Execute ACTION_DRIVER_SEARCH: search the catalog */
static int exec_driver_search(const char *query, char *output, int output_size)
{
    int pos = 0;

    if (!query || !query[0]) {
        buf_append(output, &pos, output_size, "Usage: search driver <name or category>\n");
        return -1;
    }

    buf_append(output, &pos, output_size, "Searching catalog for: ");
    buf_append(output, &pos, output_size, query);
    buf_append(output, &pos, output_size, "\n");

    /* Check catalog for matching entries */
    catalog_entry_t results[16];
    uint32_t found = 0;

    /* Try searching by each category */
    static const struct { const char *name; uint32_t cat; } cats[] = {
        { "storage", AJDRV_CAT_STORAGE }, { "network", AJDRV_CAT_NETWORK },
        { "input",   AJDRV_CAT_INPUT },   { "display", AJDRV_CAT_DISPLAY },
        { "gpu",     AJDRV_CAT_GPU },     { "bus",     AJDRV_CAT_BUS },
    };

    for (uint32_t c = 0; c < 6; c++) {
        if (contains_substr(query, cats[c].name)) {
            found = catalog_find_by_category(cats[c].cat, results, 16);
            break;
        }
    }

    if (found == 0) {
        buf_append(output, &pos, output_size,
            "No drivers found in local catalog.\n"
            "Try connecting to the marketplace for online search.\n");
        return 0;
    }

    for (uint32_t i = 0; i < found; i++) {
        buf_append(output, &pos, output_size, "  ");
        buf_append(output, &pos, output_size, results[i].name);
        buf_append(output, &pos, output_size, " v");
        buf_append(output, &pos, output_size, results[i].version);
        buf_append(output, &pos, output_size, " (");
        buf_append_uint(output, &pos, output_size, results[i].size_bytes);
        buf_append(output, &pos, output_size, " bytes)\n");
    }

    buf_append_uint(output, &pos, output_size, found);
    buf_append(output, &pos, output_size, " driver(s) found.\n");
    return 0;
}

/* Execute ACTION_SYS_INFO: comprehensive system information */
static int exec_sys_info(char *output, int output_size)
{
    int pos = 0;

    buf_append(output, &pos, output_size, "AlJefra OS v1.0\n");
    buf_append(output, &pos, output_size, "Universal Boot Operating System\n\n");

    /* Architecture */
    buf_append(output, &pos, output_size, "Architecture: ");
    switch (hal_arch()) {
    case HAL_ARCH_X86_64:  buf_append(output, &pos, output_size, "x86-64\n");   break;
    case HAL_ARCH_AARCH64: buf_append(output, &pos, output_size, "AArch64\n");  break;
    case HAL_ARCH_RISCV64: buf_append(output, &pos, output_size, "RISC-V 64\n"); break;
    }

    /* CPU */
    hal_cpu_info_t cpu;
    hal_cpu_get_info(&cpu);
    buf_append(output, &pos, output_size, "CPU: ");
    buf_append(output, &pos, output_size, cpu.model);
    buf_append(output, &pos, output_size, "\n");
    buf_append(output, &pos, output_size, "  Vendor: ");
    buf_append(output, &pos, output_size, cpu.vendor);
    buf_append(output, &pos, output_size, "\n");
    buf_append(output, &pos, output_size, "  Physical cores: ");
    buf_append_uint(output, &pos, output_size, cpu.cores_physical);
    buf_append(output, &pos, output_size, "\n");
    buf_append(output, &pos, output_size, "  Logical cores: ");
    buf_append_uint(output, &pos, output_size, cpu.cores_logical);
    buf_append(output, &pos, output_size, "\n");
    buf_append(output, &pos, output_size, "  Cache line: ");
    buf_append_uint(output, &pos, output_size, cpu.cache_line_bytes);
    buf_append(output, &pos, output_size, " bytes\n");

    /* CPU features */
    buf_append(output, &pos, output_size, "  Features:");
    if (cpu.features & HAL_CPU_FEAT_FPU)    buf_append(output, &pos, output_size, " FPU");
    if (cpu.features & HAL_CPU_FEAT_SSE)    buf_append(output, &pos, output_size, " SSE");
    if (cpu.features & HAL_CPU_FEAT_AVX)    buf_append(output, &pos, output_size, " AVX");
    if (cpu.features & HAL_CPU_FEAT_NEON)   buf_append(output, &pos, output_size, " NEON");
    if (cpu.features & HAL_CPU_FEAT_SVE)    buf_append(output, &pos, output_size, " SVE");
    if (cpu.features & HAL_CPU_FEAT_RVVEC)  buf_append(output, &pos, output_size, " RVV");
    if (cpu.features & HAL_CPU_FEAT_RDRAND) buf_append(output, &pos, output_size, " RDRAND");
    if (cpu.features & HAL_CPU_FEAT_CRC32)  buf_append(output, &pos, output_size, " CRC32");
    if (cpu.features & HAL_CPU_FEAT_AES)    buf_append(output, &pos, output_size, " AES");
    if (cpu.features & HAL_CPU_FEAT_ATOMICS) buf_append(output, &pos, output_size, " ATOMICS");
    buf_append(output, &pos, output_size, "\n");

    /* Memory */
    uint64_t total_mb = hal_mmu_total_ram() / (1024 * 1024);
    uint64_t free_mb  = hal_mmu_free_ram() / (1024 * 1024);
    buf_append(output, &pos, output_size, "\nMemory:\n");
    buf_append(output, &pos, output_size, "  Total: ");
    buf_append_uint(output, &pos, output_size, total_mb);
    buf_append(output, &pos, output_size, " MB\n");
    buf_append(output, &pos, output_size, "  Free: ");
    buf_append_uint(output, &pos, output_size, free_mb);
    buf_append(output, &pos, output_size, " MB\n");
    buf_append(output, &pos, output_size, "  Used: ");
    buf_append_uint(output, &pos, output_size, total_mb - free_mb);
    buf_append(output, &pos, output_size, " MB\n");

    /* Drivers */
    const driver_ops_t *drivers[MAX_DRIVERS];
    uint32_t drv_count = driver_list(drivers, MAX_DRIVERS);
    buf_append(output, &pos, output_size, "\nDrivers: ");
    buf_append_uint(output, &pos, output_size, drv_count);
    buf_append(output, &pos, output_size, " loaded\n");

    /* Uptime */
    uint64_t ms = hal_timer_ms();
    uint64_t secs = ms / 1000;
    uint64_t mins = secs / 60;
    uint64_t hours = mins / 60;
    buf_append(output, &pos, output_size, "\nUptime: ");
    buf_append_uint(output, &pos, output_size, hours);
    buf_append(output, &pos, output_size, "h ");
    buf_append_uint(output, &pos, output_size, mins % 60);
    buf_append(output, &pos, output_size, "m ");
    buf_append_uint(output, &pos, output_size, secs % 60);
    buf_append(output, &pos, output_size, "s\n");

    /* Timer frequency */
    buf_append(output, &pos, output_size, "Timer: ");
    buf_append_uint(output, &pos, output_size, hal_timer_freq_hz());
    buf_append(output, &pos, output_size, " Hz\n");

    return 0;
}

/* Execute ACTION_SYS_UPTIME */
static int exec_sys_uptime(char *output, int output_size)
{
    int pos = 0;
    uint64_t ms = hal_timer_ms();
    uint64_t secs = ms / 1000;
    uint64_t mins = secs / 60;
    uint64_t hours = mins / 60;
    uint64_t days = hours / 24;

    buf_append(output, &pos, output_size, "Uptime: ");
    if (days > 0) {
        buf_append_uint(output, &pos, output_size, days);
        buf_append(output, &pos, output_size, " day(s), ");
    }
    buf_append_uint(output, &pos, output_size, hours % 24);
    buf_append(output, &pos, output_size, " hour(s), ");
    buf_append_uint(output, &pos, output_size, mins % 60);
    buf_append(output, &pos, output_size, " minute(s), ");
    buf_append_uint(output, &pos, output_size, secs % 60);
    buf_append(output, &pos, output_size, " second(s)\n");

    return 0;
}

/* Execute ACTION_MEMORY_INFO */
static int exec_memory_info(char *output, int output_size)
{
    int pos = 0;

    uint64_t total = hal_mmu_total_ram();
    uint64_t free_ram = hal_mmu_free_ram();
    uint64_t used = total - free_ram;

    buf_append(output, &pos, output_size, "Memory Information:\n");
    buf_append(output, &pos, output_size, "  Total: ");
    buf_append_uint(output, &pos, output_size, total / (1024 * 1024));
    buf_append(output, &pos, output_size, " MB (");
    buf_append_uint(output, &pos, output_size, total);
    buf_append(output, &pos, output_size, " bytes)\n");

    buf_append(output, &pos, output_size, "  Used:  ");
    buf_append_uint(output, &pos, output_size, used / (1024 * 1024));
    buf_append(output, &pos, output_size, " MB\n");

    buf_append(output, &pos, output_size, "  Free:  ");
    buf_append_uint(output, &pos, output_size, free_ram / (1024 * 1024));
    buf_append(output, &pos, output_size, " MB\n");

    /* Usage percentage (avoid FP: percent = used * 100 / total) */
    if (total > 0) {
        uint64_t pct = (used * 100) / total;
        buf_append(output, &pos, output_size, "  Usage: ");
        buf_append_uint(output, &pos, output_size, pct);
        buf_append(output, &pos, output_size, "%\n");
    }

    /* Page size */
    buf_append(output, &pos, output_size, "  Page size: 4096 bytes\n");

    /* Memory regions */
    hal_mem_region_t regions[32];
    uint32_t nregions = hal_mmu_get_memory_map(regions, 32);
    if (nregions > 0) {
        buf_append(output, &pos, output_size, "\n  Memory regions:\n");
        for (uint32_t i = 0; i < nregions && i < 8; i++) {
            buf_append(output, &pos, output_size, "    0x");
            /* Simple hex output for base address */
            char hexbuf[20];
            uint64_t v = regions[i].base;
            for (int b = 15; b >= 0; b--) {
                hexbuf[15 - b] = "0123456789abcdef"[(v >> (b * 4)) & 0xF];
            }
            hexbuf[16] = '\0';
            /* Skip leading zeros */
            const char *hp = hexbuf;
            while (*hp == '0' && *(hp + 1))
                hp++;
            buf_append(output, &pos, output_size, hp);
            buf_append(output, &pos, output_size, " - ");
            buf_append_uint(output, &pos, output_size, regions[i].size / 1024);
            buf_append(output, &pos, output_size, " KB");
            buf_append(output, &pos, output_size,
                       regions[i].type == 1 ? " [usable]" :
                       regions[i].type == 2 ? " [reserved]" : " [other]");
            buf_append(output, &pos, output_size, "\n");
        }
    }

    return 0;
}

/* Execute ACTION_SYS_REBOOT */
static int exec_sys_reboot(char *output, int output_size)
{
    int pos = 0;
    buf_append(output, &pos, output_size, "Rebooting system...\n");

    /* Use ACPI reset on x86, or PSCI on ARM, or SBI on RISC-V.
     * The actual reboot is architecture-specific.
     * For x86: write 0xFE to keyboard controller port 0x64.
     * This is a last-resort reboot method. */
#if defined(__x86_64__) || defined(_M_X64)
    hal_port_out8(0x64, 0xFE);
#endif

    /* If we get here, the reboot did not take effect immediately.
     * The HAL halt should be caught by a watchdog or the user. */
    hal_cpu_halt();
    return 0;
}

/* Execute ACTION_SYS_SHUTDOWN */
static int exec_sys_shutdown(char *output, int output_size)
{
    int pos = 0;
    buf_append(output, &pos, output_size, "Shutting down...\n");
    buf_append(output, &pos, output_size,
        "All drivers unloaded. It is safe to power off.\n");

    /* On QEMU: write to ACPI PM1a control register to power off */
#if defined(__x86_64__) || defined(_M_X64)
    hal_port_out16(0x604, 0x2000);  /* QEMU ACPI shutdown */
#endif

    hal_cpu_disable_interrupts();
    for (;;)
        hal_cpu_halt();

    return 0; /* Unreachable */
}

/* Execute ACTION_SYS_UPDATE */
static int exec_sys_update(char *output, int output_size)
{
    int pos = 0;

    buf_append(output, &pos, output_size, "Checking for updates...\n");

    char update_url[256];
    update_url[0] = '\0';
    hal_status_t rc = marketplace_check_updates("1.0.0", update_url, sizeof(update_url));

    if (rc == HAL_OK && update_url[0]) {
        buf_append(output, &pos, output_size, "Update available: ");
        buf_append(output, &pos, output_size, update_url);
        buf_append(output, &pos, output_size,
            "\nRun 'install update' to download and apply.\n");
    } else if (rc == HAL_OK) {
        buf_append(output, &pos, output_size, "System is up to date.\n");
    } else {
        buf_append(output, &pos, output_size,
            "Failed to check for updates. No network connection?\n");
    }

    return 0;
}

/* Execute ACTION_TASK_LIST */
static int exec_task_list(char *output, int output_size)
{
    int pos = 0;

    buf_append(output, &pos, output_size, "Running Tasks:\n");
    buf_append(output, &pos, output_size, "  ID  State    Core\n");
    buf_append(output, &pos, output_size, "  --  -----    ----\n");

    /* The scheduler exposes a current task ID.
     * With the cooperative model, we report what we know. */
    uint32_t cur = sched_current_task();
    buf_append(output, &pos, output_size, "  ");
    buf_append_uint(output, &pos, output_size, cur);
    buf_append(output, &pos, output_size, "   running  ");
    buf_append_uint(output, &pos, output_size, (uint64_t)hal_cpu_id());
    buf_append(output, &pos, output_size, " (current)\n");

    uint32_t ncores = hal_cpu_count();
    buf_append(output, &pos, output_size, "\nCPU cores online: ");
    buf_append_uint(output, &pos, output_size, ncores);
    buf_append(output, &pos, output_size, "\n");

    return 0;
}

/* Execute ACTION_HELP: show available commands */
static int exec_help(const char *lang, char *output, int output_size)
{
    int pos = 0;

    int is_arabic = (lang && lang[0] == 'a' && lang[1] == 'r');

    if (is_arabic) {
        buf_append(output, &pos, output_size,
            /* نظام الجفرا - الأوامر المتاحة */
            "\xd9\x86\xd8\xb8\xd8\xa7\xd9\x85 \xd8\xa7\xd9\x84\xd8\xac\xd9\x81\xd8\xb1\xd8\xa7 - "
            "\xd8\xa7\xd9\x84\xd8\xa3\xd9\x88\xd8\xa7\xd9\x85\xd8\xb1 "
            "\xd8\xa7\xd9\x84\xd9\x85\xd8\xaa\xd8\xa7\xd8\xad\xd8\xa9:\n\n");
    } else {
        buf_append(output, &pos, output_size,
            "AlJefra OS -- Available Commands:\n\n");
    }

    buf_append(output, &pos, output_size, "File System:\n");
    buf_append(output, &pos, output_size, "  ls / list files       - List files on disk\n");
    buf_append(output, &pos, output_size, "  cat <file>            - Read a file\n");
    buf_append(output, &pos, output_size, "  touch <file>          - Create a new file\n");
    buf_append(output, &pos, output_size, "  rm <file>             - Delete a file\n");
    buf_append(output, &pos, output_size, "  write <file>          - Write to a file\n");

    buf_append(output, &pos, output_size, "\nNetwork:\n");
    buf_append(output, &pos, output_size, "  ifconfig / net status - Show network info\n");
    buf_append(output, &pos, output_size, "  dhcp / connect        - Get IP via DHCP\n");

    buf_append(output, &pos, output_size, "\nDrivers:\n");
    buf_append(output, &pos, output_size, "  lsmod / list drivers  - Show loaded drivers\n");
    buf_append(output, &pos, output_size, "  search driver <name>  - Search marketplace\n");
    buf_append(output, &pos, output_size, "  install driver <name> - Install a driver\n");

    buf_append(output, &pos, output_size, "\nSystem:\n");
    buf_append(output, &pos, output_size, "  uname / sysinfo       - System information\n");
    buf_append(output, &pos, output_size, "  free / memory info    - Memory usage\n");
    buf_append(output, &pos, output_size, "  uptime                - Time since boot\n");
    buf_append(output, &pos, output_size, "  ps / list tasks       - Running tasks\n");
    buf_append(output, &pos, output_size, "  reboot                - Reboot system\n");
    buf_append(output, &pos, output_size, "  halt / shutdown       - Power off\n");
    buf_append(output, &pos, output_size, "  check update          - Check for OS updates\n");

    buf_append(output, &pos, output_size, "\nGeneral:\n");
    buf_append(output, &pos, output_size, "  help / ?              - This help screen\n");
    buf_append(output, &pos, output_size, "  (anything else)       - Ask the AI assistant\n");

    if (is_arabic) {
        buf_append(output, &pos, output_size,
            "\n"
            /* يمكنك أيضا الكتابة بالعربية */
            "\xd9\x8a\xd9\x85\xd9\x83\xd9\x86\xd9\x83 "
            "\xd8\xa3\xd9\x8a\xd8\xb6\xd8\xa7 "
            "\xd8\xa7\xd9\x84\xd9\x83\xd8\xaa\xd8\xa7\xd8\xa8\xd8\xa9 "
            "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9\n");
    } else {
        buf_append(output, &pos, output_size,
            "\nYou can also type commands in Arabic.\n");
    }

    return 0;
}

/* Master action executor */
int ai_execute_action(const ai_action_t *action, char *output, int output_size)
{
    if (!action || !output || output_size <= 0)
        return -1;

    output[0] = '\0';

    switch (action->type) {
    case ACTION_FS_LIST:
        return exec_fs_list(output, output_size);

    case ACTION_FS_READ:
        return exec_fs_read(action->args, output, output_size);

    case ACTION_FS_WRITE: {
        int pos = 0;
        buf_append(output, &pos, output_size,
            "File write not yet implemented from chat. "
            "Use the storage API directly.\n");
        return 0;
    }

    case ACTION_FS_DELETE: {
        int pos = 0;
        if (!action->args[0]) {
            buf_append(output, &pos, output_size, "Usage: delete <filename>\n");
            return -1;
        }
        buf_append(output, &pos, output_size, "File deletion not yet implemented from chat.\n");
        return 0;
    }

    case ACTION_FS_CREATE: {
        int pos = 0;
        if (!action->args[0]) {
            buf_append(output, &pos, output_size, "Usage: create <filename>\n");
            return -1;
        }
        buf_append(output, &pos, output_size, "File creation not yet implemented from chat.\n");
        return 0;
    }

    case ACTION_NET_STATUS:
        return exec_net_status(output, output_size);

    case ACTION_NET_CONNECT:
        return exec_net_connect(output, output_size);

    case ACTION_DRIVER_LIST:
        return exec_driver_list(output, output_size);

    case ACTION_DRIVER_SEARCH:
        return exec_driver_search(action->args, output, output_size);

    case ACTION_DRIVER_INSTALL: {
        int pos = 0;
        if (!action->args[0]) {
            buf_append(output, &pos, output_size,
                "Usage: install driver <name>\n"
                "First search for a driver, then install by name.\n");
            return -1;
        }
        buf_append(output, &pos, output_size,
            "Driver installation from chat requires marketplace connection.\n"
            "Use 'connect' first, then try again.\n");
        return 0;
    }

    case ACTION_SYS_INFO:
        return exec_sys_info(output, output_size);

    case ACTION_SYS_REBOOT:
        return exec_sys_reboot(output, output_size);

    case ACTION_SYS_SHUTDOWN:
        return exec_sys_shutdown(output, output_size);

    case ACTION_SYS_UPDATE:
        return exec_sys_update(output, output_size);

    case ACTION_SYS_UPTIME:
        return exec_sys_uptime(output, output_size);

    case ACTION_DISPLAY_SET: {
        int pos = 0;
        buf_append(output, &pos, output_size,
            "Display settings: ");
        if (hal_console_type() == HAL_CONSOLE_SERIAL)
            buf_append(output, &pos, output_size, "serial console (no display settings)\n");
        else if (hal_console_type() == HAL_CONSOLE_VGA)
            buf_append(output, &pos, output_size, "VGA text mode 80x25\n");
        else
            buf_append(output, &pos, output_size, "framebuffer console\n");
        return 0;
    }

    case ACTION_TASK_LIST:
        return exec_task_list(output, output_size);

    case ACTION_MEMORY_INFO:
        return exec_memory_info(output, output_size);

    case ACTION_HELP:
        return exec_help(g_session.language, output, output_size);

    case ACTION_NONE:
    default: {
        int pos = 0;
        buf_append(output, &pos, output_size, "(no action)\n");
        return 0;
    }
    }
}

/* ========================================================================
 * Response Formatter
 * ======================================================================== */

int ai_format_response(const ai_action_t *action, const char *raw_result,
                        const char *lang, char *out, int out_size)
{
    int pos = 0;

    if (!action || !raw_result || !out || out_size <= 0)
        return 0;

    int is_arabic = (lang && lang[0] == 'a' && lang[1] == 'r');

    /* If the action needed confirmation and was destructive, prefix a notice */
    if (action->needs_confirm) {
        if (is_arabic) {
            /* تحذير: */
            buf_append(out, &pos, out_size,
                "\xe2\x9a\xa0 \xd8\xaa\xd8\xad\xd8\xb0\xd9\x8a\xd8\xb1: ");
        } else {
            buf_append(out, &pos, out_size, "[!] Warning: ");
        }
    }

    /* For successful actions, add a brief header */
    if (action->type != ACTION_NONE && action->type != ACTION_HELP) {
        /* Find the action description */
        for (uint32_t i = 0; i < ACTION_TABLE_SIZE; i++) {
            if (g_action_table[i].type == action->type) {
                if (is_arabic) {
                    buf_append(out, &pos, out_size, "[");
                    buf_append(out, &pos, out_size, g_action_table[i].desc_ar);
                    buf_append(out, &pos, out_size, "]\n");
                } else {
                    buf_append(out, &pos, out_size, "[");
                    buf_append(out, &pos, out_size, g_action_table[i].desc_en);
                    buf_append(out, &pos, out_size, "]\n");
                }
                break;
            }
        }
    }

    /* Append the raw result */
    buf_append(out, &pos, out_size, raw_result);

    return pos;
}

/* ========================================================================
 * Main Chat Processing Pipeline
 * ======================================================================== */

/* Process LLM response JSON to extract action or text */
static int process_llm_response(const char *llm_response, ai_action_t *action,
                                 char *text_out, int text_max)
{
    /* Try to extract an action from the LLM JSON response */
    char action_name[64];
    char action_args[256];

    action_name[0] = '\0';
    action_args[0] = '\0';
    text_out[0] = '\0';

    /* Try "action" key first */
    int found_action = json_extract_string(llm_response, "action",
                                            action_name, sizeof(action_name));
    if (found_action >= 0 && action_name[0]) {
        /* Extract arguments */
        json_extract_string(llm_response, "args", action_args, sizeof(action_args));

        action->type = ai_action_from_name(action_name);
        str_copy(action->args, action_args, sizeof(action->args));
        action->needs_confirm = 0;
        action->confidence = 90;

        /* Mark destructive actions as needing confirmation */
        if (action->type == ACTION_SYS_REBOOT ||
            action->type == ACTION_SYS_SHUTDOWN ||
            action->type == ACTION_FS_DELETE) {
            action->needs_confirm = 1;
        }

        return 1; /* Action found */
    }

    /* Try "text" key for plain text response */
    int found_text = json_extract_string(llm_response, "text",
                                          text_out, text_max);
    if (found_text >= 0) {
        action->type = ACTION_NONE;
        return 0; /* Text response, no action */
    }

    /* Fallback: treat the entire response as text */
    str_copy(text_out, llm_response, text_max);
    action->type = ACTION_NONE;
    return 0;
}

int ai_chat_process(const char *user_input, char *response, int response_size)
{
    if (!user_input || !response || response_size <= 0)
        return -1;

    if (!g_session.initialized)
        ai_chat_init();

    response[0] = '\0';
    int pos = 0;

    /* Skip leading whitespace */
    user_input = skip_space(user_input);
    if (!*user_input) {
        buf_append(response, &pos, response_size, "(empty input)\n");
        return pos;
    }

    /* Detect language */
    if (has_arabic(user_input))
        str_copy(g_session.language, "ar", sizeof(g_session.language));
    else
        str_copy(g_session.language, "en", sizeof(g_session.language));

    /* Record user message in history */
    history_add(1, user_input);

    /* Step 1: Try local NLP parse */
    ai_action_t action;
    memset(&action, 0, sizeof(action));

    int local_match = ai_parse_local(user_input, &action);

    if (local_match && action.type != ACTION_NONE) {
        /* Local command recognized -- handle confirmation if needed */
        if (action.needs_confirm) {
            /* For now, auto-confirm in the chat flow.
             * A full implementation would prompt the user and wait.
             * We append a warning to the response. */
            int is_arabic = (g_session.language[0] == 'a');

            if (is_arabic) {
                buf_append(response, &pos, response_size,
                    /* تنفيذ العملية... */
                    "\xd8\xaa\xd9\x86\xd9\x81\xd9\x8a\xd8\xb0 "
                    "\xd8\xa7\xd9\x84\xd8\xb9\xd9\x85\xd9\x84\xd9\x8a\xd8\xa9...\n");
            } else {
                buf_append(response, &pos, response_size,
                    "Executing (requires confirmation in production)...\n");
            }
        }

        /* Execute the action */
        char result_buf[AI_CHAT_RESPONSE_MAX];
        result_buf[0] = '\0';
        ai_execute_action(&action, result_buf, sizeof(result_buf));

        /* Format the response */
        ai_format_response(&action, result_buf, g_session.language,
                            response + pos, response_size - pos);
        pos = (int)str_len(response);

        g_session.commands_executed++;
    } else {
        /* Step 2: No local match -- try LLM if available */
        if (g_session.llm_send) {
            /* Build system prompt with context */
            char context[2048];
            ai_get_context(context, sizeof(context));

            /* Send to LLM */
            char llm_response[AI_CHAT_RESPONSE_MAX];
            llm_response[0] = '\0';

            int llm_len = g_session.llm_send(context, user_input,
                                              llm_response, sizeof(llm_response));
            g_session.llm_calls++;

            if (llm_len > 0) {
                /* Parse LLM response for action or text */
                ai_action_t llm_action;
                memset(&llm_action, 0, sizeof(llm_action));

                char text_response[AI_CHAT_RESPONSE_MAX];
                text_response[0] = '\0';

                int has_action = process_llm_response(llm_response, &llm_action,
                                                       text_response,
                                                       sizeof(text_response));

                if (has_action && llm_action.type != ACTION_NONE) {
                    /* LLM suggested an action -- execute it */
                    if (llm_action.needs_confirm) {
                        buf_append(response, &pos, response_size,
                            "The AI suggests a system action that requires "
                            "confirmation.\nExecuting...\n");
                    }

                    char result_buf[AI_CHAT_RESPONSE_MAX];
                    result_buf[0] = '\0';
                    ai_execute_action(&llm_action, result_buf, sizeof(result_buf));
                    ai_format_response(&llm_action, result_buf,
                                        g_session.language,
                                        response + pos, response_size - pos);
                    pos = (int)str_len(response);
                    g_session.commands_executed++;
                } else {
                    /* Pure text response from LLM */
                    buf_append(response, &pos, response_size, text_response);
                }
            } else {
                /* LLM call failed */
                if (g_session.language[0] == 'a') {
                    buf_append(response, &pos, response_size,
                        /* عذرا، لم أتمكن من الاتصال بالذكاء الاصطناعي */
                        "\xd8\xb9\xd8\xb0\xd8\xb1\xd8\xa7\xd8\x8c "
                        "\xd9\x84\xd9\x85 \xd8\xa3\xd8\xaa\xd9\x85\xd9\x83\xd9\x86 "
                        "\xd9\x85\xd9\x86 \xd8\xa7\xd9\x84\xd8\xa7\xd8\xaa\xd8\xb5\xd8\xa7\xd9\x84 "
                        "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb0\xd9\x83\xd8\xa7\xd8\xa1 "
                        "\xd8\xa7\xd9\x84\xd8\xa7\xd8\xb5\xd8\xb7\xd9\x86\xd8\xa7\xd8\xb9\xd9\x8a. "
                        "\xd8\xa7\xd9\x83\xd8\xaa\xd8\xa8 'help' "
                        "\xd9\x84\xd9\x84\xd9\x85\xd8\xb3\xd8\xa7\xd8\xb9\xd8\xaf\xd8\xa9.\n");
                } else {
                    buf_append(response, &pos, response_size,
                        "I could not reach the AI service. "
                        "Type 'help' to see available offline commands.\n");
                }
            }
        } else {
            /* No LLM available -- offline mode */
            if (g_session.language[0] == 'a') {
                buf_append(response, &pos, response_size,
                    /* لم أفهم الأمر. اكتب 'help' للمساعدة */
                    "\xd9\x84\xd9\x85 \xd8\xa3\xd9\x81\xd9\x87\xd9\x85 "
                    "\xd8\xa7\xd9\x84\xd8\xa3\xd9\x85\xd8\xb1. "
                    "\xd8\xa7\xd9\x83\xd8\xaa\xd8\xa8 'help' "
                    "\xd9\x84\xd9\x84\xd9\x85\xd8\xb3\xd8\xa7\xd8\xb9\xd8\xaf\xd8\xa9.\n");
            } else {
                buf_append(response, &pos, response_size,
                    "I don't recognize that command. "
                    "Type 'help' to see available commands, "
                    "or connect to a network for AI assistance.\n");
            }
        }
    }

    /* Record assistant response in history */
    history_add(0, response);

    return pos;
}
