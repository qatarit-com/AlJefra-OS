/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- AI Chat Engine
 *
 * The core conversational interface of the operating system.
 * Translates natural language (English + Arabic) into system actions
 * and manages bidirectional conversation with the user.
 *
 * Architecture:
 *   1. Local NLP parser handles common commands offline (fast, no network)
 *   2. When local parse yields ACTION_NONE, the message is forwarded to
 *      a remote LLM via pluggable network callbacks (keeps this module
 *      portable across architectures -- no networking code here)
 *   3. Action executor invokes HAL / driver / store APIs
 *   4. Results are formatted into human-readable responses
 *
 * Usage:
 *   ai_chat_init();
 *   ai_chat_set_llm_callback(my_send_fn);   // plug in network layer
 *   ai_chat_process("show files", response, sizeof(response));
 */

#ifndef ALJEFRA_AI_CHAT_H
#define ALJEFRA_AI_CHAT_H

#include <stdint.h>
#include "../hal/hal.h"

/* ========================================================================
 * 1. System Action Types
 * ======================================================================== */

typedef enum {
    ACTION_FS_LIST,          /* List files on storage                       */
    ACTION_FS_READ,          /* Read a file by name                         */
    ACTION_FS_WRITE,         /* Write data to a file                        */
    ACTION_FS_DELETE,        /* Delete a file                               */
    ACTION_FS_CREATE,        /* Create a new empty file                     */
    ACTION_NET_STATUS,       /* Show network status (IP, MAC, link)         */
    ACTION_NET_CONNECT,      /* Connect to network (trigger DHCP)           */
    ACTION_DRIVER_LIST,      /* List all loaded drivers                     */
    ACTION_DRIVER_SEARCH,    /* Search marketplace for a driver             */
    ACTION_DRIVER_INSTALL,   /* Install a driver from marketplace           */
    ACTION_SYS_INFO,         /* System information (arch, CPU, RAM, uptime) */
    ACTION_SYS_REBOOT,       /* Reboot the system                          */
    ACTION_SYS_SHUTDOWN,     /* Halt / power off                           */
    ACTION_SYS_UPDATE,       /* Check for OS updates                        */
    ACTION_SYS_UPTIME,       /* Show uptime                                */
    ACTION_DISPLAY_SET,      /* Change display settings                     */
    ACTION_TASK_LIST,        /* List scheduler tasks                        */
    ACTION_MEMORY_INFO,      /* Show memory usage                           */
    ACTION_HELP,             /* Show help / available commands              */
    ACTION_NONE,             /* No action -- pure chat / forward to LLM    */
    ACTION_COUNT             /* Sentinel -- total number of actions         */
} action_type_t;

/* Action descriptor -- what to do and with what arguments */
typedef struct {
    action_type_t type;
    char          args[256];     /* Action arguments (filename, search term, etc.) */
    int           needs_confirm; /* 1 = user must confirm before execution         */
    int           confidence;    /* 0-100: how confident the local parse is        */
} ai_action_t;

/* ========================================================================
 * 2. Chat Session State
 * ======================================================================== */

/* Conversation history entry */
typedef struct {
    int  is_user;           /* 1 = user message, 0 = assistant response */
    char text[512];         /* Message text (truncated if longer)        */
} chat_message_t;

/* Maximum conversation history retained in memory */
#define AI_CHAT_HISTORY_MAX   16

/* Maximum response buffer size */
#define AI_CHAT_RESPONSE_MAX  4096

/* LLM send callback -- the network layer registers this.
 * system_prompt: context about the OS state
 * user_message:  what the user said
 * response:      output buffer for LLM response
 * response_max:  size of output buffer
 * Returns: length of response, or -1 on error.
 */
typedef int (*ai_llm_send_fn)(const char *system_prompt,
                               const char *user_message,
                               char *response, uint32_t response_max);

/* Chat session */
typedef struct {
    int             initialized;
    ai_llm_send_fn  llm_send;     /* Pluggable LLM callback (NULL = offline only) */
    chat_message_t   history[AI_CHAT_HISTORY_MAX];
    uint32_t         history_count;
    uint32_t         history_head; /* Ring buffer head index                       */
    uint64_t         session_start_ms; /* Timestamp when session was initialized   */
    uint32_t         commands_executed; /* Total actions executed this session      */
    uint32_t         llm_calls;        /* Number of LLM round-trips               */
    char             username[32];     /* Optional user name for personalization   */
    char             language[8];      /* "en" or "ar" -- detected from input      */
} ai_chat_session_t;

/* ========================================================================
 * 3. Core API
 * ======================================================================== */

/* Initialize the AI chat engine.  Call once after kernel boot.
 * Returns 0 on success, -1 on failure. */
int ai_chat_init(void);

/* Register the LLM network callback.  Can be called at any time.
 * Pass NULL to disable LLM and run in offline-only mode. */
void ai_chat_set_llm_callback(ai_llm_send_fn fn);

/* Process one user message and produce a response.
 *
 * Flow:
 *   1. Detect language (English / Arabic)
 *   2. Run local NLP parser -- try to match known command patterns
 *   3. If ACTION_NONE and LLM callback is set, build system prompt
 *      and forward to LLM; parse LLM response for action JSON
 *   4. If action needs confirmation, ask user (return prompt)
 *   5. Execute the action via ai_execute_action()
 *   6. Format the result into a human-readable response
 *
 * Returns: length of response written, or -1 on error.
 */
int ai_chat_process(const char *user_input, char *response, int response_size);

/* Execute a confirmed action and write the result to output.
 * Returns 0 on success, -1 on error. */
int ai_execute_action(const ai_action_t *action, char *output, int output_size);

/* ========================================================================
 * 4. Local NLP Parser
 * ======================================================================== */

/* Parse user input locally (no network) into an action.
 * Handles English and Arabic command patterns.
 * Returns 1 if a command was recognized, 0 if ACTION_NONE. */
int ai_parse_local(const char *input, ai_action_t *action);

/* ========================================================================
 * 5. System Context (for LLM system prompt)
 * ======================================================================== */

/* Build a system context string describing the current OS state.
 * Includes: architecture, CPU, RAM, loaded drivers, network status,
 * available actions, and recent conversation history.
 * Returns: length of context written. */
int ai_get_context(char *buf, int size);

/* ========================================================================
 * 6. Response Formatting
 * ======================================================================== */

/* Format an action result into a user-friendly response.
 * lang = "en" or "ar".
 * Returns: length of formatted response. */
int ai_format_response(const ai_action_t *action, const char *raw_result,
                        const char *lang, char *out, int out_size);

/* ========================================================================
 * 7. Action Name Lookup
 * ======================================================================== */

/* Get the string name of an action type (for LLM JSON protocol) */
const char *ai_action_name(action_type_t type);

/* Parse an action name string back to action_type_t.
 * Returns ACTION_NONE if not recognized. */
action_type_t ai_action_from_name(const char *name);

/* Get the session pointer (read-only, for status display) */
const ai_chat_session_t *ai_chat_get_session(void);

#endif /* ALJEFRA_AI_CHAT_H */
