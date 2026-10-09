/**
 * @file jailbreak_detector.h
 * @brief Lightweight pure C heuristic jailbreak & adversarial prompt injection detector.
 */
#ifndef AIGATE_JAILBREAK_DETECTOR_H
#define AIGATE_JAILBREAK_DETECTOR_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JAILBREAK_ACTION_PASS = 0,
    JAILBREAK_ACTION_FLAG = 1,
    JAILBREAK_ACTION_BLOCK = 2
} jailbreak_action_t;

typedef struct {
    jailbreak_action_t action;
    int                risk_score;   /**< 0 ~ 100 composite risk score */
    char               rule_tag[64]; /**< Highest-severity matched rule tag */
    char               reason[128];  /**< Human-readable violation detail */
} jailbreak_result_t;

typedef struct jailbreak_detector jailbreak_detector_t;

/**
 * @brief Create a new jailbreak detector instance.
 * @return New instance or NULL on OOM.
 */
jailbreak_detector_t* jailbreak_detector_create(void);

/**
 * @brief Destroy a jailbreak detector instance.
 * @param d Pointer to detector instance (NULL safe).
 */
void jailbreak_detector_destroy(jailbreak_detector_t* d);

/**
 * @brief Inspect inbound prompt payload for jailbreak and adversarial injection patterns.
 * @param d Detector instance.
 * @param prompt_json Raw request JSON body or extracted prompt text.
 * @param prompt_len Length of prompt_json in bytes.
 * @param[out] out_res Written with risk evaluation results.
 * @return Action recommendation (PASS, FLAG, or BLOCK).
 */
jailbreak_action_t jailbreak_detector_inspect(jailbreak_detector_t* d,
                                              const char*           prompt_json,
                                              size_t                prompt_len,
                                              jailbreak_result_t*   out_res);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_JAILBREAK_DETECTOR_H */
