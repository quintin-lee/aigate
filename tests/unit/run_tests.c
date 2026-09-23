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
    extern void test_auth_key_unknown_neg_cache(void);
    extern void test_key_allows_model(void);
    extern void test_rl_qps_boundary(void);
    extern void test_rl_unlimited(void);
    extern void test_rl_daily_quota(void);
    extern void test_rl_reset_day(void);
    extern void test_rl_concurrent_smoke(void);
    extern void test_model_router_env_key(void);
    extern void test_model_router_missing_env_key(void);
    extern void test_model_router_multi_target_keys(void);
    extern void test_model_router_priority_selection(void);
    extern void test_model_router_round_robin(void);
    extern void test_model_router_weighted(void);
    extern void test_model_router_cb_exclusion_and_fallback(void);
    extern void test_model_router_half_open_probe_in_candidates(void);
    extern void test_upstream_200_roundtrip(void);
    extern void test_upstream_500_passthrough(void);
    extern void test_upstream_timeout(void);
    extern void test_upstream_fail_all_toggle(void);
    extern void test_um_counters_and_drain(void);
    extern void test_um_drain_fail_requeue(void);
    extern void test_um_provider_metering(void);
    extern void test_um_request_ring(void);
    extern void test_metrics_acl(void);
    extern void test_metrics_failover(void);
    extern void test_core_pipeline(void);
    extern void test_core_models_rate_limited(void);
    extern void test_core_models_list_failure_503(void);
    extern void test_core_upstream_400_passthrough(void);
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
    test_register("auth_key_neg_cache", test_auth_key_unknown_neg_cache);
    test_register("key_allows_model", test_key_allows_model);
    test_register("rl_qps_boundary", test_rl_qps_boundary);
    test_register("rl_unlimited", test_rl_unlimited);
    test_register("rl_daily_quota", test_rl_daily_quota);
    test_register("rl_reset_day", test_rl_reset_day);
    test_register("rl_concurrent", test_rl_concurrent_smoke);
    test_register("model_router_env", test_model_router_env_key);
    test_register("model_router_missing", test_model_router_missing_env_key);
    test_register("model_router_multi_target_keys", test_model_router_multi_target_keys);
    test_register("model_router_priority_selection", test_model_router_priority_selection);
    test_register("model_router_round_robin", test_model_router_round_robin);
    test_register("model_router_weighted", test_model_router_weighted);
    test_register("model_router_cb_exclusion", test_model_router_cb_exclusion_and_fallback);
    test_register("model_router_half_open_probe", test_model_router_half_open_probe_in_candidates);
    test_register("upstream_200", test_upstream_200_roundtrip);
    test_register("upstream_500", test_upstream_500_passthrough);
    test_register("upstream_timeout", test_upstream_timeout);
    test_register("upstream_fail_all", test_upstream_fail_all_toggle);
    test_register("um_counters", test_um_counters_and_drain);
    test_register("um_drain_fail_requeue", test_um_drain_fail_requeue);
    test_register("um_provider_metering", test_um_provider_metering);
    test_register("um_request_ring", test_um_request_ring);
    test_register("metrics_acl", test_metrics_acl);
    test_register("metrics_failover", test_metrics_failover);
    test_register("core_pipeline", test_core_pipeline);
    test_register("core_models_rate_limited", test_core_models_rate_limited);
    test_register("core_models_503", test_core_models_list_failure_503);
    test_register("core_upstream_400_passthrough", test_core_upstream_400_passthrough);
    test_register("provider_azure_build", test_provider_azure_build);
    test_register("provider_merge_params", test_provider_default_params_merge);

    extern void test_admin_auth(void);
    extern void test_admin_keys_lifecycle(void);
    extern void test_admin_models_lifecycle(void);
    extern void test_admin_models_multi_target(void);
    extern void test_admin_default_params_oversize_rejected(void);
    extern void test_admin_usage_query(void);
    extern void test_admin_provider_create_and_list(void);
    extern void test_admin_provider_patch_and_delete(void);
    extern void test_admin_provider_sync_failed_reported(void);
    extern void test_admin_provider_plaintext_gate(void);
    extern void test_admin_lockout(void);
    extern void test_admin_lockout_policy_env(void);
    test_register("admin_auth", test_admin_auth);
    test_register("admin_models_lifecycle", test_admin_models_lifecycle);
    test_register("admin_models_multi_target", test_admin_models_multi_target);
    test_register("admin_default_params_oversize", test_admin_default_params_oversize_rejected);
    test_register("admin_usage_query", test_admin_usage_query);
    test_register("admin_provider_create", test_admin_provider_create_and_list);
    test_register("admin_provider_patch_delete", test_admin_provider_patch_and_delete);
    test_register("admin_provider_sync_failed", test_admin_provider_sync_failed_reported);
    test_register("admin_provider_plaintext_gate", test_admin_provider_plaintext_gate);
    test_register("admin_lockout", test_admin_lockout);
    test_register("admin_lockout_policy_env", test_admin_lockout_policy_env);

    extern void test_upstream_stream_normal(void);
    extern void test_upstream_stream_silence_timeout(void);
    test_register("upstream_stream_normal", test_upstream_stream_normal);
    test_register("upstream_stream_silence_timeout", test_upstream_stream_silence_timeout);

    extern void test_stream_pipeline_normal(void);
    extern void test_stream_pipeline_early_error(void);
    extern void test_stream_pipeline_4xx_passthrough(void);
    extern void test_stream_pipeline_silence_timeout(void);
    test_register("stream_pipeline_normal", test_stream_pipeline_normal);
    test_register("stream_pipeline_early_error", test_stream_pipeline_early_error);
    test_register("stream_pipeline_4xx_passthrough", test_stream_pipeline_4xx_passthrough);
    test_register("stream_pipeline_silence_timeout", test_stream_pipeline_silence_timeout);

    extern void test_anthropic_build_system_and_defaults(void);
    extern void test_anthropic_build_params(void);
    extern void test_anthropic_resp_translation(void);
    extern void test_anthropic_bridge_streaming(void);
    extern void test_anthropic_bridge_client_abort(void);
    extern void test_anthropic_pipeline_end_to_end(void);
    test_register("anthropic_build_system", test_anthropic_build_system_and_defaults);
    test_register("anthropic_build_params", test_anthropic_build_params);
    test_register("anthropic_resp_translation", test_anthropic_resp_translation);
    test_register("anthropic_bridge_streaming", test_anthropic_bridge_streaming);
    test_register("anthropic_bridge_client_abort", test_anthropic_bridge_client_abort);
    test_register("anthropic_pipeline_end_to_end", test_anthropic_pipeline_end_to_end);

    extern void admin_ui_content(void);
    test_register("admin_ui_content", admin_ui_content);

    extern void test_deepseek_reasoning_non_streaming(void);
    extern void test_openai_cached_tokens_details(void);
    extern void test_deepseek_streaming_reasoning_and_cache(void);
    test_register("deepseek_reasoning_non_streaming", test_deepseek_reasoning_non_streaming);
    test_register("openai_cached_tokens_details", test_openai_cached_tokens_details);
    test_register("deepseek_streaming_reasoning_and_cache",
                  test_deepseek_streaming_reasoning_and_cache);

    extern void test_gemini_build_system_and_contents(void);
    extern void test_gemini_build_generation_config(void);
    extern void test_gemini_resp_translation(void);
    extern void test_gemini_resp_error_unwrapping(void);
    extern void test_gemini_streaming_bridge_chunks(void);
    extern void test_gemini_streaming_fragmented_tcp(void);
    extern void test_gemini_streaming_client_abort(void);
    test_register("gemini_build_system_and_contents", test_gemini_build_system_and_contents);
    test_register("gemini_build_generation_config", test_gemini_build_generation_config);
    test_register("gemini_resp_translation", test_gemini_resp_translation);
    test_register("gemini_resp_error_unwrapping", test_gemini_resp_error_unwrapping);
    test_register("gemini_streaming_bridge_chunks", test_gemini_streaming_bridge_chunks);
    test_register("gemini_streaming_fragmented_tcp", test_gemini_streaming_fragmented_tcp);
    test_register("gemini_streaming_client_abort", test_gemini_streaming_client_abort);

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

    extern void test_cb_normal_traffic(void);
    extern void test_cb_tripping_on_consecutive_failures(void);
    extern void test_cb_cooloff_and_half_open_probe_success(void);
    extern void test_cb_probe_failure_trips_back_to_open(void);
    extern void test_cb_open_failures_do_not_refresh_cooloff(void);
    extern void test_cb_concurrency_stress(void);
    test_register("cb_normal_traffic", test_cb_normal_traffic);
    test_register("cb_tripping", test_cb_tripping_on_consecutive_failures);
    test_register("cb_cooloff_and_probe_success", test_cb_cooloff_and_half_open_probe_success);
    test_register("cb_probe_failure_trips_back", test_cb_probe_failure_trips_back_to_open);
    test_register("cb_open_no_refresh", test_cb_open_failures_do_not_refresh_cooloff);
    test_register("cb_concurrency_stress", test_cb_concurrency_stress);

    extern void test_failover_on_500_to_backup(void);
    extern void test_failover_on_429_to_backup(void);
    extern void test_failover_circuit_breaker_tripping(void);
    test_register("failover_on_500", test_failover_on_500_to_backup);
    test_register("failover_on_429", test_failover_on_429_to_backup);
    test_register("failover_cb_tripping", test_failover_circuit_breaker_tripping);

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
