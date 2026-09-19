/** @file run_tests.c
 *  @brief assert-based unit test runner (no framework; spec section 6).
 *
 *  Each test file defines plain (non-static) test functions using TEST_CASE
 *  from run_tests.h; this file keeps the registry. Exit code = number of
 *  failed test functions.
 */
#include "run_tests.h"
#include <stdio.h>

int g_failures = 0;

typedef void (*test_fn)(void);
static struct {
    const char* name;
    test_fn     fn;
} g_tests[256];
static int g_n_tests = 0;

void
test_register(const char* name, test_fn fn)
{
    if (g_n_tests < (int)(sizeof g_tests / sizeof g_tests[0])) {
        g_tests[g_n_tests].name = name;
        g_tests[g_n_tests].fn = fn;
        g_n_tests++;
    }
}

int
main(void)
{
    extern void test_log_smoke(void);
    extern void test_log_concurrent(void);
    extern void test_sha256_kat(void);
    extern void test_sha256_equal(void);
    extern void test_config_defaults(void);
    extern void test_config_missing_required(void);
    extern void test_config_bad_master_key(void);
    extern void test_lru_eviction_order(void);
    extern void test_lru_recency_refresh(void);
    extern void test_lru_replace_and_invalidate(void);
    extern void test_lru_concurrent_smoke(void);
    extern void test_pg_fake_key_lifecycle(void);
    extern void test_pg_fake_model_lifecycle(void);
    extern void test_pg_fake_multi_target_model(void);
    extern void test_pg_fake_usage_flush_and_query(void);
    extern void test_pg_migrate_noop_for_fake(void);
    extern void test_pg_real_roundtrip(void);
    extern void test_secret_roundtrip(void);
    extern void test_secret_tamper_and_wrong_key(void);
    extern void test_secret_hex_to_bytes(void);
    extern void test_auth_key_resolve_normal(void);
    extern void test_auth_key_unknown_revoked_expired(void);
    extern void test_key_allows_model(void);
    extern void test_rl_qps_boundary(void);
    extern void test_rl_unlimited(void);
    extern void test_rl_daily_quota(void);
    extern void test_rl_reset_day(void);
    extern void test_rl_concurrent_smoke(void);
    extern void test_model_router_env_key(void);
    extern void test_model_router_missing_env_key(void);
    extern void test_upstream_200_roundtrip(void);
    extern void test_upstream_500_passthrough(void);
    extern void test_upstream_timeout(void);
    extern void test_upstream_fail_all_toggle(void);
    extern void test_um_counters_and_drain(void);
    extern void test_metrics_acl(void);
    extern void test_core_pipeline(void);
    extern void test_provider_azure_build(void);
    extern void test_provider_default_params_merge(void);
    test_register("log_smoke", test_log_smoke);
    test_register("log_concurrent", test_log_concurrent);
    test_register("sha256_kat", test_sha256_kat);
    test_register("sha256_equal", test_sha256_equal);
    test_register("config_defaults", test_config_defaults);
    test_register("config_missing_required", test_config_missing_required);
    test_register("config_bad_master_key", test_config_bad_master_key);
    test_register("lru_eviction_order", test_lru_eviction_order);
    test_register("lru_recency_refresh", test_lru_recency_refresh);
    test_register("lru_replace_and_invalidate", test_lru_replace_and_invalidate);
    test_register("lru_concurrent_smoke", test_lru_concurrent_smoke);
    test_register("pg_fake_key_lifecycle", test_pg_fake_key_lifecycle);
    test_register("pg_fake_model_lifecycle", test_pg_fake_model_lifecycle);
    test_register("pg_fake_multi_target_model", test_pg_fake_multi_target_model);
    test_register("pg_fake_usage_flush_and_query", test_pg_fake_usage_flush_and_query);
    test_register("pg_migrate_noop_for_fake", test_pg_migrate_noop_for_fake);
    test_register("pg_real_roundtrip", test_pg_real_roundtrip);
    test_register("secret_roundtrip", test_secret_roundtrip);
    test_register("secret_tamper", test_secret_tamper_and_wrong_key);
    test_register("secret_hex", test_secret_hex_to_bytes);
    test_register("auth_key_resolve", test_auth_key_resolve_normal);
    test_register("auth_key_flags", test_auth_key_unknown_revoked_expired);
    test_register("key_allows_model", test_key_allows_model);
    test_register("rl_qps_boundary", test_rl_qps_boundary);
    test_register("rl_unlimited", test_rl_unlimited);
    test_register("rl_daily_quota", test_rl_daily_quota);
    test_register("rl_reset_day", test_rl_reset_day);
    test_register("rl_concurrent", test_rl_concurrent_smoke);
    test_register("model_router_env", test_model_router_env_key);
    test_register("model_router_missing", test_model_router_missing_env_key);
    test_register("upstream_200", test_upstream_200_roundtrip);
    test_register("upstream_500", test_upstream_500_passthrough);
    test_register("upstream_timeout", test_upstream_timeout);
    test_register("upstream_fail_all", test_upstream_fail_all_toggle);
    test_register("um_counters", test_um_counters_and_drain);
    test_register("metrics_acl", test_metrics_acl);
    test_register("core_pipeline", test_core_pipeline);
    test_register("provider_azure_build", test_provider_azure_build);
    test_register("provider_merge_params", test_provider_default_params_merge);

    extern void test_admin_auth(void);
    extern void test_admin_keys_lifecycle(void);
    extern void test_admin_models_lifecycle(void);
    extern void test_admin_usage_query(void);
    test_register("admin_auth", test_admin_auth);
    test_register("admin_keys_lifecycle", test_admin_keys_lifecycle);
    test_register("admin_models_lifecycle", test_admin_models_lifecycle);
    test_register("admin_usage_query", test_admin_usage_query);

    extern void test_upstream_stream_normal(void);
    extern void test_upstream_stream_silence_timeout(void);
    test_register("upstream_stream_normal", test_upstream_stream_normal);
    test_register("upstream_stream_silence_timeout", test_upstream_stream_silence_timeout);

    extern void test_stream_pipeline_normal(void);
    extern void test_stream_pipeline_early_error(void);
    extern void test_stream_pipeline_silence_timeout(void);
    test_register("stream_pipeline_normal", test_stream_pipeline_normal);
    test_register("stream_pipeline_early_error", test_stream_pipeline_early_error);
    test_register("stream_pipeline_silence_timeout", test_stream_pipeline_silence_timeout);

    extern void test_anthropic_build_system_and_defaults(void);
    extern void test_anthropic_build_params(void);
    extern void test_anthropic_resp_translation(void);
    extern void test_anthropic_bridge_streaming(void);
    extern void test_anthropic_pipeline_end_to_end(void);
    test_register("anthropic_build_system", test_anthropic_build_system_and_defaults);
    test_register("anthropic_build_params", test_anthropic_build_params);
    test_register("anthropic_resp_translation", test_anthropic_resp_translation);
    test_register("anthropic_bridge_streaming", test_anthropic_bridge_streaming);
    test_register("anthropic_pipeline_end_to_end", test_anthropic_pipeline_end_to_end);

    extern void admin_ui_content(void);
    test_register("admin_ui_content", admin_ui_content);

    extern void test_deepseek_reasoning_non_streaming(void);
    extern void test_openai_cached_tokens_details(void);
    extern void test_deepseek_streaming_reasoning_and_cache(void);
    test_register("deepseek_reasoning_non_streaming", test_deepseek_reasoning_non_streaming);
    test_register("openai_cached_tokens_details", test_openai_cached_tokens_details);
    test_register("deepseek_streaming_reasoning_and_cache", test_deepseek_streaming_reasoning_and_cache);

    extern void test_gemini_build_system_and_contents(void);
    extern void test_gemini_build_generation_config(void);
    extern void test_gemini_resp_translation(void);
    extern void test_gemini_resp_error_unwrapping(void);
    extern void test_gemini_streaming_bridge_chunks(void);
    extern void test_gemini_streaming_fragmented_tcp(void);
    test_register("gemini_build_system_and_contents", test_gemini_build_system_and_contents);
    test_register("gemini_build_generation_config", test_gemini_build_generation_config);
    test_register("gemini_resp_translation", test_gemini_resp_translation);
    test_register("gemini_resp_error_unwrapping", test_gemini_resp_error_unwrapping);
    test_register("gemini_streaming_bridge_chunks", test_gemini_streaming_bridge_chunks);
    test_register("gemini_streaming_fragmented_tcp", test_gemini_streaming_fragmented_tcp);

    extern void test_openai_embeddings_build(void);
    extern void test_openai_embeddings_parse(void);
    extern void test_gemini_embeddings_build_single(void);
    extern void test_gemini_embeddings_build_batch(void);
    extern void test_gemini_embeddings_parse_single(void);
    extern void test_gemini_embeddings_parse_batch(void);
    extern void test_gemini_embeddings_parse_error(void);
    extern void test_embeddings_pipeline_e2e(void);
    test_register("openai_embeddings_build", test_openai_embeddings_build);
    test_register("openai_embeddings_parse", test_openai_embeddings_parse);
    test_register("gemini_embeddings_build_single", test_gemini_embeddings_build_single);
    test_register("gemini_embeddings_build_batch", test_gemini_embeddings_build_batch);
    test_register("gemini_embeddings_parse_single", test_gemini_embeddings_parse_single);
    test_register("gemini_embeddings_parse_batch", test_gemini_embeddings_parse_batch);
    test_register("gemini_embeddings_parse_error", test_gemini_embeddings_parse_error);
    test_register("embeddings_pipeline_e2e", test_embeddings_pipeline_e2e);

    int failed = 0;
    for (int i = 0; i < g_n_tests; i++) {
        g_failures = 0;
        g_tests[i].fn();
        if (g_failures > 0) {
            failed++;
            fprintf(stderr, "FAILED: %s\n", g_tests[i].name);
        }
    }
    printf("PASS: %d/%d test(s), %d failure(s)\n", g_n_tests - failed, g_n_tests, failed);
    return failed;
}
