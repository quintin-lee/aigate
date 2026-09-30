#ifndef AIGATE_PROMPT_TEMPLATE_H
#define AIGATE_PROMPT_TEMPLATE_H

#include <stddef.h>
#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROMPT_MODE_PREPEND = 0,
    PROMPT_MODE_APPEND = 1,
    PROMPT_MODE_OVERRIDE = 2
} prompt_inject_mode_t;

typedef struct {
    const char*          system_template;
    prompt_inject_mode_t mode;
    const char*          prefix_user_prompt;
    const char*          suffix_user_prompt;
} prompt_template_t;

/**
 * @brief Parse mode string ("prepend", "append", "override") to enum.
 * @return PROMPT_MODE_PREPEND on default or unrecognized.
 */
prompt_inject_mode_t prompt_mode_from_str(const char* str);

/**
 * @brief Format mode enum to static string ("prepend", "append", "override").
 */
const char* prompt_mode_to_str(prompt_inject_mode_t mode);

/**
 * @brief Interpolate variables (${date}, ${time}, ${timestamp}, ${model}, ${key_name}) into template.
 * @return Newly allocated string (caller must free), or NULL on allocation error.
 */
char* prompt_template_expand_vars(const char* tmpl, const char* model, const char* key_name);

/**
 * @brief Apply prompt template to a JSON request body.
 * @param tmpl Template settings.
 * @param model Model name for interpolation.
 * @param key_name API Key identifier for interpolation.
 * @param jbody Parsed request JSON body.
 * @param out_modified_json Newly serialized JSON string (caller must free) on success.
 * @param out_len Length of out_modified_json.
 * @return 0 on success, non-zero if unmodified or error.
 */
int prompt_template_apply(const prompt_template_t* tmpl,
                          const char*              model,
                          const char*              key_name,
                          json_t*                  jbody,
                          char**                   out_modified_json,
                          size_t*                  out_len);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_PROMPT_TEMPLATE_H */
