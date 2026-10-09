/** @file run_tests.c
 *  @brief assert-based unit test runner (no framework; spec section 6).
 *
 *  Each test file defines plain (non-static) test functions using TEST_CASE
 *  from run_tests.h; this file keeps the registry. Exit code = number of
 *  failed test functions.
 */
#include "run_tests.h"
#include <stdio.h>
#include <string.h>

int g_failures = 0;

/** @brief Single test-case function pointer. */
typedef void (*test_fn)(void);
static struct {
    const char* name;
    test_fn     fn;
} g_tests[512];
static int g_n_tests = 0;

/** @brief Register a test case.
 *  @param name Case name. @param fn Case function. */
void
test_register(const char* name, test_fn fn)
{
    if (g_n_tests < (int)(sizeof g_tests / sizeof g_tests[0])) {
        g_tests[g_n_tests].name = name;
        g_tests[g_n_tests].fn = fn;
        g_n_tests++;
    }
}

/** @brief Run all registered cases in order.
 *  @return Number of failed cases; 0 means all passed. */
int
main(int argc, char** argv)
{
    extern void test_log_smoke(void);
    extern void test_log_concurrent(void);
    extern void test_log_json_and_level_filtering(void);
    extern void test_sha256_kat(void);
    extern void test_sha256_equal(void);
    extern void test_config_defaults(void);
    extern void test_config_missing_required(void);
    extern void test_config_bad_master_key(void);
    extern void test_config_worker_threads_and_p0(void);
    extern void test_config_p1_features(void);
    extern void test_config_ssl(void);
    extern void test_lru_eviction_order(void);
    extern void test_lru_recency_refresh(void);
    extern void test_lru_replace_and_invalidate(void);
    extern void test_lru_concurrent_smoke(void);
    extern void test_pg_fake_key_lifecycle(void);
    extern void test_pg_fake_model_lifecycle(void);
    extern void test_pg_fake_multi_target_model(void);
    extern void test_pg_fake_usage_flush_and_query(void);
    extern void test_pg_fake_request_flush_and_query(void);
    extern void test_pg_fake_reasoning_tokens(void);
    extern void test_pg_fake_guardrails_lifecycle(void);
    extern void test_pg_fake_key_budget_fields(void);
    extern void test_pg_fake_group_budget(void);
    extern void test_pg_fake_prompt_template(void);
    extern void test_guardrails_ac_basic(void);
    extern void test_guardrails_ac_overlapping(void);
    extern void test_guardrails_ac_edge_cases(void);
    extern void test_guardrails_pii_masking(void);
    extern void test_guardrails_inbound_json_inspection(void);
    extern void test_guardrails_webhook_unit(void);
    extern void test_pii_checksum_algorithms(void);
    extern void test_pii_session_map_and_partial_masking(void);
    extern void test_pii_inbound_transformation(void);
    extern void test_pii_outbound_restoration(void);
    extern void test_budget_enforce_unlimited(void);
    extern void test_budget_enforce_key_cost_limit(void);
    extern void test_budget_enforce_key_token_limit(void);
    extern void test_budget_enforce_group_cost_limit(void);
    extern void test_budget_enforce_rollover_and_reset(void);
    extern void test_pg_migrate_noop_for_fake(void);
    extern void test_pg_real_roundtrip(void);
    extern void test_pg_real_provider_crud(void);
    extern void test_pg_real_groups_and_cost(void);
    extern void test_pg_store_rotate_master_key(void);
    extern void test_secret_roundtrip(void);
    extern void test_secret_tamper_and_wrong_key(void);
    extern void test_secret_hex_to_bytes(void);
    extern void test_secret_rotate(void);
    extern void test_auth_key_resolve_normal(void);
    extern void test_auth_key_flags(void);
    extern void test_auth_key_unknown_revoked_expired(void);
    extern void test_auth_key_unknown_neg_cache(void);
    extern void test_key_allows_model(void);
    extern void test_credential_extraction_variants(void);
    extern void test_rl_qps_boundary(void);
    extern void test_rl_unlimited(void);
    extern void test_rl_daily_quota(void);
    extern void test_rl_reset_day(void);
    extern void test_rl_concurrent_smoke(void);
    extern void test_rl_redis_fail_closed(void);
    extern void test_rl_redis_fail_open(void);
    extern void test_model_router_env_key(void);
    extern void test_model_router_missing_env_key(void);
    extern void test_model_router_multi_target_keys(void);
    extern void test_model_router_priority_selection(void);
    extern void test_model_router_round_robin(void);
    extern void test_model_router_weighted(void);
    extern void test_model_router_cb_exclusion_and_fallback(void);
    extern void test_model_router_half_open_probe_in_candidates(void);
    extern void test_model_router_target_provider_override(void);
    extern void test_upstream_200_roundtrip(void);
    extern void test_upstream_500_passthrough(void);
    extern void test_upstream_timeout(void);
    extern void test_upstream_fail_all_toggle(void);
    extern void test_um_counters_and_drain(void);
    extern void test_um_drain_fail_requeue(void);
    extern void test_um_provider_metering(void);
    extern void test_um_request_ring(void);
    extern void test_um_high_volume_drain(void);
    extern void test_metrics_acl(void);
    extern void test_metrics_failover(void);
    extern void test_metrics_ttft(void);
    extern void test_metrics_concurrency_rejection_and_alloc(void);
    extern void test_core_pipeline(void);
    extern void test_core_models_rate_limited(void);
    extern void test_core_models_list_failure_503(void);
    extern void test_core_upstream_400_passthrough(void);
    extern void test_core_guardrail_block(void);
    extern void test_core_guardrail_pii_masking(void);
    extern void test_core_monthly_budget_cost_limit(void);
    extern void test_provider_azure_build(void);
    extern void test_provider_default_params_merge(void);
    test_register("log_smoke", test_log_smoke);
    test_register("log_concurrent", test_log_concurrent);
    test_register("log_json_and_level_filtering", test_log_json_and_level_filtering);
    test_register("sha256_kat", test_sha256_kat);
    test_register("sha256_equal", test_sha256_equal);
    test_register("config_defaults", test_config_defaults);
    test_register("config_missing_required", test_config_missing_required);
    test_register("config_bad_master_key", test_config_bad_master_key);
    test_register("config_worker_threads_and_p0", test_config_worker_threads_and_p0);
    test_register("config_p1_features", test_config_p1_features);
    test_register("config_ssl", test_config_ssl);
    test_register("lru_eviction_order", test_lru_eviction_order);
    test_register("lru_recency_refresh", test_lru_recency_refresh);
    test_register("lru_replace_and_invalidate", test_lru_replace_and_invalidate);
    test_register("lru_concurrent_smoke", test_lru_concurrent_smoke);
    test_register("pg_fake_key_lifecycle", test_pg_fake_key_lifecycle);
    test_register("pg_fake_model_lifecycle", test_pg_fake_model_lifecycle);
    test_register("pg_fake_multi_target_model", test_pg_fake_multi_target_model);
    test_register("pg_fake_usage_flush_and_query", test_pg_fake_usage_flush_and_query);
    test_register("pg_fake_request_flush_and_query", test_pg_fake_request_flush_and_query);
    test_register("pg_fake_reasoning_tokens", test_pg_fake_reasoning_tokens);
    test_register("pg_fake_guardrails_lifecycle", test_pg_fake_guardrails_lifecycle);
    test_register("pg_fake_key_budget_fields", test_pg_fake_key_budget_fields);
    test_register("pg_fake_group_budget", test_pg_fake_group_budget);
    test_register("pg_fake_prompt_template", test_pg_fake_prompt_template);
    test_register("guardrails_ac_basic", test_guardrails_ac_basic);
    test_register("guardrails_ac_overlapping", test_guardrails_ac_overlapping);
    test_register("guardrails_ac_edge_cases", test_guardrails_ac_edge_cases);
    test_register("guardrails_pii_masking", test_guardrails_pii_masking);
    test_register("guardrails_inbound_json_inspection", test_guardrails_inbound_json_inspection);
    test_register("guardrails_webhook_unit", test_guardrails_webhook_unit);
    test_register("pii_checksum_algorithms", test_pii_checksum_algorithms);
    test_register("pii_session_map_and_partial_masking", test_pii_session_map_and_partial_masking);
    test_register("pii_inbound_transformation", test_pii_inbound_transformation);
    test_register("pii_outbound_restoration", test_pii_outbound_restoration);
    test_register("budget_enforce_unlimited", test_budget_enforce_unlimited);
    test_register("budget_enforce_key_cost_limit", test_budget_enforce_key_cost_limit);
    test_register("budget_enforce_key_token_limit", test_budget_enforce_key_token_limit);
    test_register("budget_enforce_group_cost_limit", test_budget_enforce_group_cost_limit);
    test_register("budget_enforce_rollover_and_reset", test_budget_enforce_rollover_and_reset);

    test_register("pg_migrate_noop_for_fake", test_pg_migrate_noop_for_fake);
    test_register("pg_real_roundtrip", test_pg_real_roundtrip);
    test_register("pg_real_provider_crud", test_pg_real_provider_crud);
    test_register("pg_real_groups_and_cost", test_pg_real_groups_and_cost);
    test_register("pg_store_rotate_master_key", test_pg_store_rotate_master_key);
    test_register("secret_roundtrip", test_secret_roundtrip);
    test_register("secret_tamper", test_secret_tamper_and_wrong_key);
    test_register("secret_hex", test_secret_hex_to_bytes);
    test_register("secret_rotate", test_secret_rotate);
    test_register("auth_key_resolve", test_auth_key_resolve_normal);
    test_register("auth_key_flags", test_auth_key_unknown_revoked_expired);
    test_register("auth_key_neg_cache", test_auth_key_unknown_neg_cache);
    test_register("key_allows_model", test_key_allows_model);
    test_register("credential_extraction_variants", test_credential_extraction_variants);
    test_register("rl_qps_boundary", test_rl_qps_boundary);
    test_register("rl_unlimited", test_rl_unlimited);
    test_register("rl_daily_quota", test_rl_daily_quota);
    test_register("rl_reset_day", test_rl_reset_day);
    test_register("rl_concurrent", test_rl_concurrent_smoke);
    test_register("rl_redis_fail_closed", test_rl_redis_fail_closed);
    test_register("rl_redis_fail_open", test_rl_redis_fail_open);
    test_register("model_router_env", test_model_router_env_key);
    test_register("model_router_missing", test_model_router_missing_env_key);
    test_register("model_router_multi_target_keys", test_model_router_multi_target_keys);
    test_register("model_router_priority_selection", test_model_router_priority_selection);
    test_register("model_router_round_robin", test_model_router_round_robin);
    test_register("model_router_weighted", test_model_router_weighted);
    test_register("model_router_cb_exclusion", test_model_router_cb_exclusion_and_fallback);
    test_register("model_router_half_open_probe", test_model_router_half_open_probe_in_candidates);
    test_register("model_router_target_provider", test_model_router_target_provider_override);
    test_register("upstream_200", test_upstream_200_roundtrip);
    test_register("upstream_500", test_upstream_500_passthrough);
    test_register("upstream_timeout", test_upstream_timeout);
    test_register("upstream_fail_all", test_upstream_fail_all_toggle);
    test_register("um_counters", test_um_counters_and_drain);
    test_register("um_drain_fail_requeue", test_um_drain_fail_requeue);
    test_register("um_provider_metering", test_um_provider_metering);
    test_register("um_request_ring", test_um_request_ring);
    test_register("um_high_volume_drain", test_um_high_volume_drain);
    test_register("metrics_acl", test_metrics_acl);
    test_register("metrics_failover", test_metrics_failover);
    test_register("metrics_ttft", test_metrics_ttft);
    test_register("metrics_concurrency_rejection_and_alloc",
                  test_metrics_concurrency_rejection_and_alloc);
    test_register("core_pipeline", test_core_pipeline);
    test_register("core_models_rate_limited", test_core_models_rate_limited);
    test_register("core_models_503", test_core_models_list_failure_503);
    test_register("core_upstream_400_passthrough", test_core_upstream_400_passthrough);
    test_register("core_guardrail_block", test_core_guardrail_block);
    test_register("core_guardrail_pii_masking", test_core_guardrail_pii_masking);
    test_register("core_monthly_budget_cost_limit", test_core_monthly_budget_cost_limit);

    extern void test_responses_non_openai_400(void);
    extern void test_responses_pipeline_200(void);
    extern void test_responses_missing_model_400(void);
    test_register("responses_non_openai_400", test_responses_non_openai_400);
    test_register("responses_pipeline_200", test_responses_pipeline_200);
    test_register("responses_missing_model_400", test_responses_missing_model_400);

    extern void test_anthropic_native_pipeline_200(void);
    extern void test_anthropic_native_stream_pipeline_200(void);
    extern void test_anthropic_native_non_anthropic_400(void);
    extern void test_gemini_native_pipeline_200(void);
    extern void test_gemini_native_non_gemini_400(void);
    extern void test_gemini_native_stream_pipeline_200(void);
    test_register("anthropic_native_pipeline_200", test_anthropic_native_pipeline_200);
    test_register("anthropic_native_stream_pipeline_200",
                  test_anthropic_native_stream_pipeline_200);
    test_register("anthropic_native_non_anthropic_400", test_anthropic_native_non_anthropic_400);
    test_register("gemini_native_pipeline_200", test_gemini_native_pipeline_200);
    test_register("gemini_native_non_gemini_400", test_gemini_native_non_gemini_400);
    test_register("gemini_native_stream_pipeline_200", test_gemini_native_stream_pipeline_200);

    test_register("provider_azure_build", test_provider_azure_build);
    test_register("provider_merge_params", test_provider_default_params_merge);

    extern void test_openai_responses_build(void);
    extern void test_openai_responses_parse_usage_nonstream(void);
    extern void test_openai_responses_parse_usage_stream(void);
    test_register("openai_responses_build", test_openai_responses_build);
    test_register("openai_responses_parse_usage_nonstream",
                  test_openai_responses_parse_usage_nonstream);
    test_register("openai_responses_parse_usage_stream", test_openai_responses_parse_usage_stream);

    extern void test_probe_plan_openai_family(void);
    extern void test_probe_plan_anthropic_suffixes(void);
    extern void test_probe_plan_gemini(void);
    extern void test_probe_plan_errors(void);
    extern void test_probe_transport_ok(void);
    extern void test_probe_transport_key_invalid(void);
    extern void test_probe_transport_unreachable(void);
    test_register("probe_plan_openai_family", test_probe_plan_openai_family);
    test_register("probe_plan_anthropic", test_probe_plan_anthropic_suffixes);
    test_register("probe_plan_gemini", test_probe_plan_gemini);
    test_register("probe_plan_errors", test_probe_plan_errors);
    test_register("probe_transport_ok", test_probe_transport_ok);
    test_register("probe_transport_key_invalid", test_probe_transport_key_invalid);
    test_register("probe_transport_unreachable", test_probe_transport_unreachable);
    extern void test_admin_auth(void);
    extern void test_admin_keys_lifecycle(void);
    extern void test_admin_models_lifecycle(void);
    extern void test_admin_models_multi_target(void);
    extern void test_admin_default_params_oversize_rejected(void);
    extern void test_admin_usage_query(void);
    extern void test_admin_usage_requests_query(void);
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
    test_register("admin_usage_requests_query", test_admin_usage_requests_query);
    test_register("admin_provider_create", test_admin_provider_create_and_list);
    test_register("admin_provider_patch_delete", test_admin_provider_patch_and_delete);
    test_register("admin_provider_sync_failed", test_admin_provider_sync_failed_reported);
    test_register("admin_provider_plaintext_gate", test_admin_provider_plaintext_gate);
    test_register("admin_lockout", test_admin_lockout);
    test_register("admin_lockout_policy_env", test_admin_lockout_policy_env);

    extern void test_admin_provider_test_ok(void);
    extern void test_admin_provider_test_key_invalid(void);
    extern void test_admin_provider_test_unverified_404(void);
    extern void test_admin_provider_test_env_missing(void);
    extern void test_admin_provider_test_unknown_type(void);
    extern void test_admin_provider_test_not_found(void);
    test_register("admin_provider_test_ok", test_admin_provider_test_ok);
    test_register("admin_provider_test_key_invalid", test_admin_provider_test_key_invalid);
    test_register("admin_provider_test_unverified", test_admin_provider_test_unverified_404);
    test_register("admin_provider_test_env_missing", test_admin_provider_test_env_missing);
    test_register("admin_provider_test_unknown_type", test_admin_provider_test_unknown_type);
    test_register("admin_provider_test_not_found", test_admin_provider_test_not_found);

    extern void test_admin_groups_crud(void);
    extern void test_admin_models_pricing(void);
    extern void test_cost_from_rows_pure(void);
    extern void test_admin_cost_endpoint(void);
    extern void test_admin_pagination(void);
    extern void test_admin_key_budgets_and_guardrails(void);
    extern void test_admin_group_budget(void);
    extern void test_admin_guardrails_crud_and_reload(void);
    extern void test_admin_pii_endpoints(void);
    extern void test_admin_prompt_template_crud(void);
    test_register("admin_groups_crud", test_admin_groups_crud);
    test_register("admin_models_pricing", test_admin_models_pricing);
    test_register("cost_from_rows_pure", test_cost_from_rows_pure);
    test_register("admin_cost_endpoint", test_admin_cost_endpoint);
    test_register("admin_pagination", test_admin_pagination);
    test_register("admin_key_budgets_and_guardrails", test_admin_key_budgets_and_guardrails);
    test_register("admin_group_budget", test_admin_group_budget);
    test_register("admin_guardrails_crud_and_reload", test_admin_guardrails_crud_and_reload);
    test_register("admin_pii_endpoints", test_admin_pii_endpoints);
    test_register("admin_prompt_template_crud", test_admin_prompt_template_crud);

    extern void test_upstream_stream_normal(void);
    extern void test_upstream_stream_silence_timeout(void);
    test_register("upstream_stream_normal", test_upstream_stream_normal);
    test_register("upstream_stream_silence_timeout", test_upstream_stream_silence_timeout);

    extern void test_stream_pipeline_normal(void);
    extern void test_stream_pipeline_early_error(void);
    extern void test_stream_pipeline_4xx_passthrough(void);
    extern void test_stream_pipeline_silence_timeout(void);
    extern void test_stream_pipeline_cache_dual_interop(void);
    test_register("stream_pipeline_normal", test_stream_pipeline_normal);
    test_register("stream_pipeline_early_error", test_stream_pipeline_early_error);
    test_register("stream_pipeline_4xx_passthrough", test_stream_pipeline_4xx_passthrough);
    test_register("stream_pipeline_silence_timeout", test_stream_pipeline_silence_timeout);
    test_register("stream_pipeline_cache_dual_interop", test_stream_pipeline_cache_dual_interop);

    extern void test_anthropic_build_system_and_defaults(void);
    extern void test_anthropic_build_params(void);
    extern void test_anthropic_resp_translation(void);
    extern void test_anthropic_bridge_streaming(void);
    extern void test_anthropic_bridge_client_abort(void);
    extern void test_anthropic_pipeline_end_to_end(void);
    extern void test_anthropic_sniff_usage_json(void);
    extern void test_anthropic_sniff_streaming_sse(void);
    test_register("anthropic_build_system", test_anthropic_build_system_and_defaults);
    test_register("anthropic_build_params", test_anthropic_build_params);
    test_register("anthropic_resp_translation", test_anthropic_resp_translation);
    test_register("anthropic_bridge_streaming", test_anthropic_bridge_streaming);
    test_register("anthropic_bridge_client_abort", test_anthropic_bridge_client_abort);
    test_register("anthropic_pipeline_end_to_end", test_anthropic_pipeline_end_to_end);
    test_register("anthropic_sniff_usage_json", test_anthropic_sniff_usage_json);
    test_register("anthropic_sniff_streaming_sse", test_anthropic_sniff_streaming_sse);

    extern void test_anthropic_tools_request_build(void);
    extern void test_anthropic_tool_use_response_parse(void);
    extern void test_anthropic_mixed_text_and_tool_use(void);
    extern void test_anthropic_tool_result_message_build(void);
    extern void test_anthropic_sse_tool_call_stream(void);
    extern void test_anthropic_tool_choice_required_mapping(void);
    test_register("anthropic_tools_request_build", test_anthropic_tools_request_build);
    test_register("anthropic_tool_use_response_parse", test_anthropic_tool_use_response_parse);
    test_register("anthropic_mixed_text_and_tool_use", test_anthropic_mixed_text_and_tool_use);
    test_register("anthropic_tool_result_message_build", test_anthropic_tool_result_message_build);
    test_register("anthropic_sse_tool_call_stream", test_anthropic_sse_tool_call_stream);
    test_register("anthropic_tool_choice_required_mapping",
                  test_anthropic_tool_choice_required_mapping);

    extern void test_anthropic_vision_single_image(void);
    extern void test_anthropic_vision_text_only_string(void);
    extern void test_anthropic_vision_multi_image(void);
    test_register("anthropic_vision_single_image", test_anthropic_vision_single_image);
    test_register("anthropic_vision_text_only_string", test_anthropic_vision_text_only_string);
    test_register("anthropic_vision_multi_image", test_anthropic_vision_multi_image);

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
    extern void test_gemini_sniff_usage_json(void);
    extern void test_gemini_sniff_streaming_sse(void);
    test_register("gemini_build_system_and_contents", test_gemini_build_system_and_contents);
    test_register("gemini_build_generation_config", test_gemini_build_generation_config);
    test_register("gemini_resp_translation", test_gemini_resp_translation);
    test_register("gemini_resp_error_unwrapping", test_gemini_resp_error_unwrapping);
    test_register("gemini_streaming_bridge_chunks", test_gemini_streaming_bridge_chunks);
    test_register("gemini_streaming_fragmented_tcp", test_gemini_streaming_fragmented_tcp);
    test_register("gemini_streaming_client_abort", test_gemini_streaming_client_abort);
    test_register("gemini_sniff_usage_json", test_gemini_sniff_usage_json);
    test_register("gemini_sniff_streaming_sse", test_gemini_sniff_streaming_sse);

    extern void test_gemini_tools_request_build(void);
    extern void test_gemini_function_call_response_parse(void);
    extern void test_gemini_tool_result_name_lookup(void);
    extern void test_gemini_tool_result_name_missing_fallback(void);
    extern void test_gemini_multi_tool_calls_response(void);
    extern void test_gemini_sse_function_call_stream(void);
    test_register("gemini_tools_request_build", test_gemini_tools_request_build);
    test_register("gemini_function_call_response_parse", test_gemini_function_call_response_parse);
    test_register("gemini_tool_result_name_lookup", test_gemini_tool_result_name_lookup);
    test_register("gemini_tool_result_name_missing_fallback",
                  test_gemini_tool_result_name_missing_fallback);
    test_register("gemini_multi_tool_calls_response", test_gemini_multi_tool_calls_response);
    test_register("gemini_sse_function_call_stream", test_gemini_sse_function_call_stream);

    extern void test_gemini_vision_single_image(void);
    extern void test_gemini_vision_unknown_mime_fallback(void);
    extern void test_gemini_vision_text_only_string(void);
    test_register("gemini_vision_single_image", test_gemini_vision_single_image);
    test_register("gemini_vision_unknown_mime_fallback", test_gemini_vision_unknown_mime_fallback);
    test_register("gemini_vision_text_only_string", test_gemini_vision_text_only_string);

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
    extern void test_cb_redis_fail_open(void);
    test_register("cb_normal_traffic", test_cb_normal_traffic);
    test_register("cb_tripping", test_cb_tripping_on_consecutive_failures);
    test_register("cb_cooloff_and_probe_success", test_cb_cooloff_and_half_open_probe_success);
    test_register("cb_probe_failure_trips_back", test_cb_probe_failure_trips_back_to_open);
    test_register("cb_open_no_refresh", test_cb_open_failures_do_not_refresh_cooloff);
    test_register("cb_concurrency_stress", test_cb_concurrency_stress);
    test_register("cb_redis_fail_open", test_cb_redis_fail_open);

    extern void test_failover_on_500_to_backup(void);
    extern void test_failover_on_429_to_backup(void);
    extern void test_failover_circuit_breaker_tripping(void);
    extern void test_concurrency_semaphore(void);
    extern void test_failover_concurrency_limiting(void);
    test_register("failover_on_500", test_failover_on_500_to_backup);
    test_register("failover_on_429", test_failover_on_429_to_backup);
    test_register("failover_cb_tripping", test_failover_circuit_breaker_tripping);
    test_register("concurrency_semaphore", test_concurrency_semaphore);
    test_register("failover_concurrency_limiting", test_failover_concurrency_limiting);

    extern void test_redis_pool_invalid_args(void);
    extern void test_redis_client_eval_and_pool_live(void);
    test_register("redis_pool_invalid_args", test_redis_pool_invalid_args);
    test_register("redis_client_eval_and_pool_live", test_redis_client_eval_and_pool_live);

    extern void test_event_bus_lifecycle(void);
    extern void test_event_bus_sub_unsub(void);
    extern void test_event_bus_publish_pop(void);
    extern void test_event_bus_overflow_drop(void);
    extern void test_event_bus_helpers(void);
    test_register("event_bus_lifecycle", test_event_bus_lifecycle);
    test_register("event_bus_sub_unsub", test_event_bus_sub_unsub);
    test_register("event_bus_publish_pop", test_event_bus_publish_pop);
    test_register("event_bus_overflow_drop", test_event_bus_overflow_drop);
    test_register("event_bus_helpers", test_event_bus_helpers);

    extern void test_health_prober_lifecycle(void);
    extern void test_health_prober_state_transitions(void);
    extern void test_health_prober_json_serialization(void);
    test_register("health_prober_lifecycle", test_health_prober_lifecycle);
    test_register("health_prober_state_transitions", test_health_prober_state_transitions);
    test_register("health_prober_json_serialization", test_health_prober_json_serialization);

    extern void test_admin_provider_health_and_probe(void);
    test_register("admin_provider_health_and_probe", test_admin_provider_health_and_probe);

    extern void test_response_cache_all(void);
    test_register("response_cache_all", test_response_cache_all);

    extern void test_admin_cache_stats_and_purge(void);
    test_register("admin_cache_stats_and_purge", test_admin_cache_stats_and_purge);

    extern void test_prompt_template_suite(void);
    test_register("prompt_template_suite", test_prompt_template_suite);

    extern void test_filter_chain_suite(void);
    test_register("filter_chain_suite", test_filter_chain_suite);

    extern void test_latency_tracker_suite(void);
    test_register("latency_tracker_suite", test_latency_tracker_suite);

    extern void test_model_router_adaptive_suite(void);
    test_register("model_router_adaptive_suite", test_model_router_adaptive_suite);

    extern void test_upstream_hedged_suite(void);
    test_register("upstream_hedged_suite", test_upstream_hedged_suite);

    extern void test_w3c_traceparent_parsing(void);
    test_register("w3c_traceparent_parsing", test_w3c_traceparent_parsing);

    extern void test_span_lifecycle_and_timing(void);
    test_register("span_lifecycle_and_timing", test_span_lifecycle_and_timing);

    extern void test_trace_tail_sampling_decision(void);
    test_register("trace_tail_sampling_decision", test_trace_tail_sampling_decision);

    extern void test_trace_ring_buffer_operations(void);
    test_register("trace_ring_buffer_operations", test_trace_ring_buffer_operations);

    extern void test_otlp_json_serialization(void);
    test_register("otlp_json_serialization", test_otlp_json_serialization);

    extern void test_tracer_recent_cache(void);
    test_register("tracer_recent_cache", test_tracer_recent_cache);

    extern void test_tracer_manager_lifecycle(void);
    test_register("tracer_manager_lifecycle", test_tracer_manager_lifecycle);

    extern void test_admin_traces_endpoints(void);
    test_register("admin_traces_endpoints", test_admin_traces_endpoints);

    extern void test_shadow_rule_matching_and_sampling(void);
    test_register("shadow_rule_matching_and_sampling", test_shadow_rule_matching_and_sampling);

    extern void test_shadow_queue_push_pop_overflow(void);
    test_register("shadow_queue_push_pop_overflow", test_shadow_queue_push_pop_overflow);

    extern void test_shadow_eval_cache_circular_and_stats(void);
    test_register("shadow_eval_cache_circular_and_stats",
                  test_shadow_eval_cache_circular_and_stats);

    extern void test_shadow_pairing_primary_first(void);
    test_register("shadow_pairing_primary_first", test_shadow_pairing_primary_first);

    extern void test_shadow_pairing_shadow_first(void);
    test_register("shadow_pairing_shadow_first", test_shadow_pairing_shadow_first);

    extern void test_shadow_engine_lifecycle_and_task_submission(void);
    test_register("shadow_engine_lifecycle_and_task_submission",
                  test_shadow_engine_lifecycle_and_task_submission);

    extern void test_canary_routing_and_circuit_breaker_rollback(void);
    test_register("canary_routing_and_circuit_breaker_rollback",
                  test_canary_routing_and_circuit_breaker_rollback);

    extern void test_pipeline_canary_routing_header_injection(void);
    test_register("pipeline_canary_routing_header_injection",
                  test_pipeline_canary_routing_header_injection);

    extern void test_pipeline_traffic_shadowing_cloning_and_pairing(void);
    test_register("pipeline_traffic_shadowing_cloning_and_pairing",
                  test_pipeline_traffic_shadowing_cloning_and_pairing);

    extern void test_admin_shadow_endpoints(void);
    test_register("admin_shadow_endpoints", test_admin_shadow_endpoints);

    extern void test_compressor_fast_token_estimate(void);
    test_register("compressor_fast_token_estimate", test_compressor_fast_token_estimate);

    extern void test_compressor_whitespace_sanitization(void);
    test_register("compressor_whitespace_sanitization", test_compressor_whitespace_sanitization);

    extern void test_compressor_history_windowing_and_safety(void);
    test_register("compressor_history_windowing_and_safety",
                  test_compressor_history_windowing_and_safety);

    extern void test_compressor_sentence_density_pruning_and_cache(void);
    test_register("compressor_sentence_density_pruning_and_cache",
                  test_compressor_sentence_density_pruning_and_cache);

    extern void test_compressor_rule_serialization_and_match(void);
    test_register("compressor_rule_serialization_and_match",
                  test_compressor_rule_serialization_and_match);

    extern void test_pipeline_prompt_compression_and_headers(void);
    test_register("pipeline_prompt_compression_and_headers",
                  test_pipeline_prompt_compression_and_headers);

    extern void test_admin_compressor_endpoints(void);
    test_register("admin_compressor_endpoints", test_admin_compressor_endpoints);

    extern void test_admin_cache_optimizer_endpoints(void);
    test_register("admin_cache_optimizer_endpoints", test_admin_cache_optimizer_endpoints);

    extern void test_cache_optimizer_sort_tools(void);
    test_register("cache_optimizer_sort_tools", test_cache_optimizer_sort_tools);

    extern void test_cache_optimizer_normalize_whitespace(void);
    test_register("cache_optimizer_normalize_whitespace",
                  test_cache_optimizer_normalize_whitespace);

    extern void test_cache_optimizer_sink_dynamic_system(void);
    test_register("cache_optimizer_sink_dynamic_system", test_cache_optimizer_sink_dynamic_system);

    extern void test_cache_optimizer_inject_anthropic_breakpoints(void);
    test_register("cache_optimizer_inject_anthropic_breakpoints",
                  test_cache_optimizer_inject_anthropic_breakpoints);

    extern void test_cache_optimizer_cache_and_stats(void);
    test_register("cache_optimizer_cache_and_stats", test_cache_optimizer_cache_and_stats);

    extern void test_cache_optimizer_rule_match(void);
    test_register("cache_optimizer_rule_match", test_cache_optimizer_rule_match);

    extern void test_pipeline_cache_optimizer_and_headers(void);
    test_register("pipeline_cache_optimizer_and_headers",
                  test_pipeline_cache_optimizer_and_headers);

    extern void test_transport_healthz_and_ready(void);
    test_register("transport_healthz_and_ready", test_transport_healthz_and_ready);

    extern void test_transport_client_ip_resolution(void);
    test_register("transport_client_ip_resolution", test_transport_client_ip_resolution);

    extern void test_transport_cors_and_security_headers(void);
    test_register("transport_cors_and_security_headers", test_transport_cors_and_security_headers);

    extern void test_transport_dynamic_config_reload(void);
    test_register("transport_dynamic_config_reload", test_transport_dynamic_config_reload);

    extern void test_audit_event_serialization_and_snapshots(void);
    test_register("audit_event_serialization_and_snapshots",
                  test_audit_event_serialization_and_snapshots);

    extern void test_audit_ring_buffer_concurrency_and_drops(void);
    test_register("audit_ring_buffer_concurrency_and_drops",
                  test_audit_ring_buffer_concurrency_and_drops);

    extern void test_audit_file_worker_and_rotation(void);
    test_register("audit_file_worker_and_rotation", test_audit_file_worker_and_rotation);

    extern void test_audit_webhook_worker_and_retry(void);
    test_register("audit_webhook_worker_and_retry", test_audit_webhook_worker_and_retry);

    extern void test_config_audit_parameters(void);
    test_register("config_audit_parameters", test_config_audit_parameters);

    extern void test_audit_metrics_exposition(void);
    test_register("audit_metrics_exposition", test_audit_metrics_exposition);

    extern void test_audit_pipeline_hook_recording(void);
    test_register("audit_pipeline_hook_recording", test_audit_pipeline_hook_recording);

    extern void test_pg_audit_violations_crud(void);
    test_register("pg_audit_violations_crud", test_pg_audit_violations_crud);

    extern void test_audit_live_ring_query_recent(void);
    test_register("audit_live_ring_query_recent", test_audit_live_ring_query_recent);

    extern void test_sla_degradation_state_machine(void);
    test_register("sla_degradation_state_machine", test_sla_degradation_state_machine);

    extern void test_model_router_sla_fallback_redirection(void);
    test_register("model_router_sla_fallback_redirection",
                  test_model_router_sla_fallback_redirection);

    extern void test_admin_audit_and_sla_endpoints(void);
    test_register("admin_audit_and_sla_endpoints", test_admin_audit_and_sla_endpoints);

    extern void test_admin_ui_contains_audit_forensic_drawer(void);
    test_register("admin_ui_contains_audit_forensic_drawer",
                  test_admin_ui_contains_audit_forensic_drawer);

    extern void test_jailbreak_detector_instruction_override(void);
    test_register("jailbreak_detector_instruction_override",
                  test_jailbreak_detector_instruction_override);

    extern void test_jailbreak_detector_persona_and_obfuscation(void);
    test_register("jailbreak_detector_persona_and_obfuscation",
                  test_jailbreak_detector_persona_and_obfuscation);

    extern void test_filter_chain_jailbreak_blocking(void);
    test_register("filter_chain_jailbreak_blocking", test_filter_chain_jailbreak_blocking);

    extern void test_filter_chain_jailbreak_clean_pass(void);
    test_register("filter_chain_jailbreak_clean_pass", test_filter_chain_jailbreak_clean_pass);

    extern void test_watermark_encode_decode_roundtrip(void);
    test_register("watermark_encode_decode_roundtrip", test_watermark_encode_decode_roundtrip);

    extern void test_watermark_mixed_chinese_and_truncation(void);
    test_register("watermark_mixed_chinese_and_truncation",
                  test_watermark_mixed_chinese_and_truncation);

    extern void test_watermark_pipeline_openai_outbound(void);
    test_register("watermark_pipeline_openai_outbound", test_watermark_pipeline_openai_outbound);

    extern void test_watermark_pipeline_anthropic_outbound(void);
    test_register("watermark_pipeline_anthropic_outbound",
                  test_watermark_pipeline_anthropic_outbound);

    extern void test_audit_hash_chain_sign_and_verify(void);
    test_register("audit_hash_chain_sign_and_verify", test_audit_hash_chain_sign_and_verify);

    extern void test_audit_hash_chain_tamper_detection(void);
    test_register("audit_hash_chain_tamper_detection", test_audit_hash_chain_tamper_detection);

    extern void test_audit_hash_chain_deletion_detection(void);
    test_register("audit_hash_chain_deletion_detection", test_audit_hash_chain_deletion_detection);

    extern void test_admin_watermark_decode_endpoint(void);
    test_register("admin_watermark_decode_endpoint", test_admin_watermark_decode_endpoint);

    extern void test_admin_audit_chain_verify_endpoint(void);
    test_register("admin_audit_chain_verify_endpoint", test_admin_audit_chain_verify_endpoint);

    extern void test_admin_ui_watermark_and_chain_elements(void);
    test_register("admin_ui_watermark_and_chain_elements",
                  test_admin_ui_watermark_and_chain_elements);

    int ran = 0;
    int failed = 0;
    for (int i = 0; i < g_n_tests; i++) {
        if (argc > 1 && strstr(g_tests[i].name, argv[1]) == NULL) {
            continue;
        }
        ran++;
        g_failures = 0;
        printf("=== TEST: %s ===\n", g_tests[i].name);
        fflush(stdout);
        g_tests[i].fn();
        if (g_failures > 0) {
            failed++;
            fprintf(stderr, "FAILED: %s\n", g_tests[i].name);
        }
    }
    printf("PASS: %d/%d test(s), %d failure(s)\n", ran - failed, ran, failed);
    return failed;
}
