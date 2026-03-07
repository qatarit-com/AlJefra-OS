/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Local/Remote LLM bridge
 *
 * Provides a practical callback for ai_chat.c. The bridge is local-first:
 * it tries a tiny Ollama-compatible model endpoint first, then can be
 * extended later for remote/cloud backends.
 */

#ifndef ALJEFRA_AI_BRIDGE_H
#define ALJEFRA_AI_BRIDGE_H

#include <stdint.h>

int ai_bridge_init(void);
int ai_bridge_send(const char *system_prompt,
                   const char *user_message,
                   char *response,
                   uint32_t response_max);
const char *ai_bridge_status(void);

#endif /* ALJEFRA_AI_BRIDGE_H */
