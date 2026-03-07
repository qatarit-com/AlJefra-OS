/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Local/Remote LLM bridge implementation */

#include "ai_bridge.h"
#include "fs.h"
#include "dhcp.h"
#include "../net/tcp.h"
#include "../net/dns.h"
#include "../lib/string.h"
#include "../hal/hal.h"

#define AI_BRIDGE_PORT_DEFAULT 11434
#define AI_BRIDGE_TIMEOUT_MS   12000

typedef struct {
    char host[64];
    uint16_t port;
    char model[64];
    char backend[16];
} ai_bridge_config_t;

static ai_bridge_config_t g_cfg;
static char g_status[128] = "offline local assistant";

static void bridge_set_status(const char *msg)
{
    if (!msg)
        return;
    str_copy(g_status, msg, sizeof(g_status));
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int parse_u16(const char *s, uint16_t *out)
{
    uint32_t val = 0;
    int seen = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10u + (uint32_t)(*s - '0');
        if (val > 65535u)
            return -1;
        s++;
        seen = 1;
    }
    if (!seen)
        return -1;
    *out = (uint16_t)val;
    return 0;
}

static int parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t octet = 0;
        int seen = 0;
        while (*s >= '0' && *s <= '9') {
            octet = octet * 10u + (uint32_t)(*s - '0');
            if (octet > 255u)
                return -1;
            s++;
            seen = 1;
        }
        if (!seen)
            return -1;
        ip = (ip << 8) | octet;
        if (i < 3) {
            if (*s != '.')
                return -1;
            s++;
        }
    }
    if (*s != '\0')
        return -1;
    *out = ip;
    return 0;
}

static void json_escape(char *dst, uint32_t dst_max, const char *src)
{
    uint32_t pos = 0;
    if (dst_max == 0)
        return;
    while (*src && pos + 2 < dst_max) {
        char c = *src++;
        if (c == '"' || c == '\\') {
            dst[pos++] = '\\';
            dst[pos++] = c;
        } else if (c == '\n') {
            dst[pos++] = '\\';
            dst[pos++] = 'n';
        } else if (c == '\r') {
            dst[pos++] = '\\';
            dst[pos++] = 'r';
        } else if (c == '\t') {
            dst[pos++] = '\\';
            dst[pos++] = 't';
        } else {
            dst[pos++] = c;
        }
    }
    dst[pos] = '\0';
}

static void append_str(char **p, char *end, const char *s)
{
    while (*s && *p < end)
        *(*p)++ = *s++;
}

static void append_u32(char **p, char *end, uint32_t v)
{
    char tmp[16];
    int n = 0;
    if (v == 0) {
        if (*p < end)
            *(*p)++ = '0';
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0 && *p < end)
        *(*p)++ = tmp[--n];
}

static int parse_http_status(const char *response, uint32_t len)
{
    uint32_t i = 0;
    int status = 0;
    if (len < 12)
        return -1;
    while (i < len && response[i] != ' ')
        i++;
    if (i + 4 >= len)
        return -1;
    i++;
    for (int j = 0; j < 3; j++) {
        char c = response[i + (uint32_t)j];
        if (c < '0' || c > '9')
            return -1;
        status = status * 10 + (c - '0');
    }
    return status;
}

static const char *find_http_body(const char *response, uint32_t len)
{
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (response[i] == '\r' && response[i + 1] == '\n' &&
            response[i + 2] == '\r' && response[i + 3] == '\n')
            return &response[i + 4];
    }
    return (const char *)0;
}

static int extract_json_string_after_key(const char *body, uint32_t len,
                                         const char *key,
                                         char *out, uint32_t out_max)
{
    uint32_t key_len = str_len(key);
    if (!body || !key || !out || out_max == 0)
        return -1;

    for (uint32_t i = 0; i + key_len + 3 < len; i++) {
        if (memcmp(&body[i], key, key_len) != 0)
            continue;
        i += key_len;
        while (i < len && body[i] != ':')
            i++;
        if (i >= len)
            return -1;
        i++;
        while (i < len && (body[i] == ' ' || body[i] == '\t'))
            i++;
        if (i >= len || body[i] != '"')
            return -1;
        i++;
        uint32_t pos = 0;
        while (i < len && pos + 1 < out_max) {
            char c = body[i++];
            if (c == '"') {
                out[pos] = '\0';
                return (int)pos;
            }
            if (c == '\\' && i < len) {
                char esc = body[i++];
                if (esc == 'n') c = '\n';
                else if (esc == 'r') c = '\r';
                else if (esc == 't') c = '\t';
                else c = esc;
            }
            out[pos++] = c;
        }
        out[pos] = '\0';
        return (int)pos;
    }
    return -1;
}

static void parse_config_line(const char *line)
{
    char value[96];
    const char *eq = line;
    uint32_t key_len = 0;

    while (*eq && *eq != '=')
        eq++;
    if (*eq != '=')
        return;

    key_len = (uint32_t)(eq - line);
    eq++;
    str_copy(value, skip_spaces(eq), sizeof(value));

    if (key_len == 4 && memcmp(line, "host", 4) == 0)
        str_copy(g_cfg.host, value, sizeof(g_cfg.host));
    else if (key_len == 4 && memcmp(line, "port", 4) == 0)
        parse_u16(value, &g_cfg.port);
    else if (key_len == 5 && memcmp(line, "model", 5) == 0)
        str_copy(g_cfg.model, value, sizeof(g_cfg.model));
    else if (key_len == 7 && memcmp(line, "backend", 7) == 0)
        str_copy(g_cfg.backend, value, sizeof(g_cfg.backend));
}

static void load_bridge_config(void)
{
    char buf[384];
    int fd = fs_open("ai.conf");
    if (fd < 0)
        return;

    int64_t rd = fs_read(fd, buf, 0, sizeof(buf) - 1);
    fs_close(fd);
    if (rd <= 0)
        return;

    buf[rd] = '\0';
    const char *cur = buf;
    while (*cur) {
        char line[128];
        uint32_t n = 0;
        while (cur[n] && cur[n] != '\n' && cur[n] != '\r' && n + 1 < sizeof(line)) {
            line[n] = cur[n];
            n++;
        }
        line[n] = '\0';
        if (line[0] && line[0] != '#')
            parse_config_line(line);
        cur += n;
        while (*cur == '\n' || *cur == '\r')
            cur++;
    }
}

static uint32_t resolve_host_ip(void)
{
    uint32_t ip = 0;
    const dhcp_config_t *lease = dhcp_get_config();

    if (ci_eq(g_cfg.host, "gateway")) {
        if (lease)
            return lease->gateway;
        return 0;
    }

    if (parse_ipv4(g_cfg.host, &ip) == 0)
        return ip;

    if (lease && lease->dns &&
        dns_resolve(g_cfg.host, lease->dns, &ip) == HAL_OK)
        return ip;

    return 0;
}

int ai_bridge_init(void)
{
    str_copy(g_cfg.host, "gateway", sizeof(g_cfg.host));
    str_copy(g_cfg.model, "qwen2.5:0.5b", sizeof(g_cfg.model));
    str_copy(g_cfg.backend, "ollama", sizeof(g_cfg.backend));
    g_cfg.port = AI_BRIDGE_PORT_DEFAULT;
    load_bridge_config();
    bridge_set_status("local-first assistant ready");
    return 0;
}

int ai_bridge_send(const char *system_prompt,
                   const char *user_message,
                   char *response,
                   uint32_t response_max)
{
    tcp_conn_t conn;
    char escaped_system[2048];
    char escaped_user[1024];
    char body[6144];
    char request[7168];
    char http_response[8192];
    char extracted[2048];
    char *p;
    char *end;
    uint32_t remote_ip;
    int32_t sent;
    int32_t got;
    int status;
    const char *resp_body;

    if (!system_prompt || !user_message || !response || response_max == 0)
        return -1;

    response[0] = '\0';

    if (!ci_eq(g_cfg.backend, "ollama")) {
        bridge_set_status("unsupported AI backend");
        str_copy(response, "Local AI backend is not supported yet.", response_max);
        return (int)str_len(response);
    }

    remote_ip = resolve_host_ip();
    if (remote_ip == 0) {
        bridge_set_status("local AI server unresolved");
        str_copy(response,
                 "Local tiny AI is not reachable yet. Connect networking or set ai.conf.",
                 response_max);
        return (int)str_len(response);
    }

    if (tcp_connect(&conn, remote_ip, g_cfg.port) != HAL_OK) {
        bridge_set_status("local AI server offline");
        str_copy(response,
                 "Local tiny AI server is offline. Start the Ollama-compatible backend.",
                 response_max);
        return (int)str_len(response);
    }

    json_escape(escaped_system, sizeof(escaped_system), system_prompt);
    json_escape(escaped_user, sizeof(escaped_user), user_message);

    p = body;
    end = body + sizeof(body) - 1;
    append_str(&p, end, "{\"model\":\"");
    append_str(&p, end, g_cfg.model);
    append_str(&p, end, "\",\"messages\":[{\"role\":\"system\",\"content\":\"");
    append_str(&p, end, escaped_system);
    append_str(&p, end, "\"},{\"role\":\"user\",\"content\":\"");
    append_str(&p, end, escaped_user);
    append_str(&p, end, "\"}],\"stream\":false}");
    *p = '\0';

    p = request;
    end = request + sizeof(request) - 1;
    append_str(&p, end, "POST /api/chat HTTP/1.1\r\nHost: ");
    append_str(&p, end, g_cfg.host);
    append_str(&p, end, "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
    append_u32(&p, end, str_len(body));
    append_str(&p, end, "\r\n\r\n");
    append_str(&p, end, body);
    *p = '\0';

    sent = tcp_send(&conn, request, str_len(request));
    if (sent < 0) {
        tcp_close(&conn);
        bridge_set_status("AI request failed");
        str_copy(response, "I could not reach the local AI backend.", response_max);
        return (int)str_len(response);
    }

    got = tcp_recv(&conn, http_response, sizeof(http_response) - 1, AI_BRIDGE_TIMEOUT_MS);
    tcp_close(&conn);
    if (got <= 0) {
        bridge_set_status("AI response timeout");
        str_copy(response, "The local AI backend did not answer in time.", response_max);
        return (int)str_len(response);
    }
    http_response[got] = '\0';

    status = parse_http_status(http_response, (uint32_t)got);
    if (status != 200) {
        bridge_set_status("AI backend HTTP error");
        str_copy(response, "The local AI backend returned an error.", response_max);
        return (int)str_len(response);
    }

    resp_body = find_http_body(http_response, (uint32_t)got);
    if (!resp_body) {
        bridge_set_status("AI backend malformed");
        str_copy(response, "The local AI backend response was malformed.", response_max);
        return (int)str_len(response);
    }

    if (extract_json_string_after_key(resp_body, (uint32_t)str_len(resp_body),
                                      "\"content\"", extracted, sizeof(extracted)) > 0) {
        str_copy(response, extracted, response_max);
        bridge_set_status("local tiny AI online");
        return (int)str_len(response);
    }

    str_copy(response, "The local AI backend answered, but I could not parse it.", response_max);
    bridge_set_status("AI response parse failed");
    return (int)str_len(response);
}

const char *ai_bridge_status(void)
{
    return g_status;
}
