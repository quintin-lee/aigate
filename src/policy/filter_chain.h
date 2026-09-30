#ifndef AIGATE_FILTER_CHAIN_H
#define AIGATE_FILTER_CHAIN_H

#include "aigate_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FILTER_CONTINUE = 0, /**< Continue executing next filter in the chain. */
    FILTER_STOP = 1      /**< Stop execution; error/response was written to client. */
} filter_action_t;

/** @brief Inbound filter function signature. */
typedef filter_action_t (*req_filter_fn)(chat_req_t* q);

/**
 * @brief Execute the standard inbound middleware filter chain:
 *        1. Guardrails content moderation (Aho-Corasick & PII scan)
 *        2. Prompt template dynamic injection & variable expansion
 * @param q Request context.
 * @return FILTER_CONTINUE to proceed with request; FILTER_STOP if interrupted.
 */
filter_action_t filter_chain_execute_inbound(chat_req_t* q);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_FILTER_CHAIN_H */
