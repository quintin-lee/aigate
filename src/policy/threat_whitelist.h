/**
 * @file threat_whitelist.h
 * @brief Dynamic threat defense whitelist engine for AI guardrail & jailbreak bypass.
 */
#ifndef AIGATE_THREAT_WHITELIST_H
#define AIGATE_THREAT_WHITELIST_H

#include "store/pg_store.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct threat_whitelist threat_whitelist_t;

/**
 * @brief Create a new in-memory threat defense whitelist engine.
 * @return Allocated engine instance or NULL on failure.
 */
threat_whitelist_t* threat_whitelist_create(void);

/**
 * @brief Destroy a threat defense whitelist engine and release all resources.
 * @param tw Whitelist engine instance (NULL safe).
 */
void threat_whitelist_destroy(threat_whitelist_t* tw);

/**
 * @brief Add or update a threat defense whitelist rule.
 * @param tw  Whitelist engine instance.
 * @param rec Rule definition.
 * @return 0 on success, -1 on error.
 */
int threat_whitelist_add(threat_whitelist_t* tw, const threat_whitelist_rec_t* rec);

/**
 * @brief Remove a threat defense whitelist rule by its rule_id.
 * @param tw      Whitelist engine instance.
 * @param rule_id Unique primary key rule ID.
 * @return 0 on success, -1 if not found or error.
 */
int threat_whitelist_remove(threat_whitelist_t* tw, int64_t rule_id);

/**
 * @brief Atomically reload all whitelist rules from an array.
 * @param tw    Whitelist engine instance.
 * @param rules Array of rule records.
 * @param count Number of rules in array.
 * @return 0 on success, -1 on error.
 */
int threat_whitelist_load(threat_whitelist_t* tw, const threat_whitelist_rec_t* rules, size_t count);

/**
 * @brief Check whether a request is bypassed by an active whitelist rule.
 *
 * Checks if any active, unexpired whitelist rule matches the given key_id, model, and rule_tag.
 *
 * @param tw              Whitelist engine instance.
 * @param key_id          Client API key ID (0 if unauthenticated).
 * @param model           Model name (may be NULL or empty).
 * @param rule_tag        Threat detector rule tag that was triggered (e.g. "roleplay_dan").
 * @param matched_rule_id Optional output pointer to store the matching rule_id (may be NULL).
 * @return true if bypassed; false otherwise.
 */
bool threat_whitelist_is_bypassed(threat_whitelist_t* tw,
                                  uint64_t            key_id,
                                  const char*         model,
                                  const char*         rule_tag,
                                  int64_t*            matched_rule_id);

/**
 * @brief Retrieve a snapshot copy of current whitelist rules.
 * @param tw     Whitelist engine instance.
 * @param out    Destination array buffer.
 * @param cap    Maximum entries out can hold.
 * @param out_n  Written with number of rules copied.
 * @return 0 on success, -1 on error.
 */
int threat_whitelist_list(threat_whitelist_t*     tw,
                          threat_whitelist_rec_t* out,
                          int                     cap,
                          int*                    out_n);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_THREAT_WHITELIST_H */
