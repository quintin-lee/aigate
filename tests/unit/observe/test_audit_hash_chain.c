#include "audit_hash_chain.h"
#include "run_tests.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

TEST_CASE(test_audit_hash_chain_sign_and_verify)
{
    const char*             secret = "test_audit_chain_secret_2026";
    audit_hash_chain_ctx_t* ctx = audit_hash_chain_create(secret);
    TEST_ASSERT(ctx != NULL, "audit_hash_chain_create failed");

    char tmppath[] = "/tmp/aigate_test_audit_chain_XXXXXX";
    int  fd = mkstemp(tmppath);
    TEST_ASSERT(fd >= 0, "mkstemp failed");
    FILE* fp = fdopen(fd, "w");
    TEST_ASSERT(fp != NULL, "fdopen failed");

    /* 写入 10 条连续签名的审计行 */
    for (int i = 1; i <= 10; i++) {
        char event_raw[256];
        snprintf(
            event_raw,
            sizeof event_raw,
            "{\"trace_id\":\"tr_%02d\",\"key_id\":%d,\"model\":\"gpt-4o\",\"http_status\":200}",
            i,
            1000 + i);
        char signed_line[512];
        int  rc = audit_hash_chain_sign(ctx, event_raw, signed_line, sizeof signed_line);
        TEST_ASSERT(rc == 0, "audit_hash_chain_sign failed for #%d", i);
        fprintf(fp, "%s\n", signed_line);
    }
    fclose(fp);

    /* 验证完整性 */
    uint64_t verified = 0, broken = 0;
    char     errmsg[256] = {0};
    int vrc = audit_hash_chain_verify_file(ctx, tmppath, &verified, &broken, errmsg, sizeof errmsg);
    TEST_ASSERT(vrc == 0, "Expected file integrity pass, got err: %s", errmsg);
    TEST_ASSERT(verified == 10, "Should verify 10 lines, got %lu", (unsigned long)verified);
    TEST_ASSERT(broken == 0, "Broken seq should be 0");

    audit_hash_chain_destroy(ctx);
    unlink(tmppath);
}

TEST_CASE(test_audit_hash_chain_tamper_detection)
{
    const char*             secret = "test_audit_chain_secret_2026";
    audit_hash_chain_ctx_t* ctx = audit_hash_chain_create(secret);
    TEST_ASSERT(ctx != NULL, "audit_hash_chain_create failed");

    char tmppath[] = "/tmp/aigate_test_tamper_XXXXXX";
    int  fd = mkstemp(tmppath);
    TEST_ASSERT(fd >= 0, "mkstemp failed");
    FILE* fp = fdopen(fd, "w");
    TEST_ASSERT(fp != NULL, "fdopen failed");

    /* 写入 5 条正常数据，人为篡改第 3 条的内容 */
    for (int i = 1; i <= 5; i++) {
        char event_raw[256];
        snprintf(event_raw, sizeof event_raw, "{\"trace_id\":\"tr_%d\",\"status\":200}", i);
        char signed_line[512];
        audit_hash_chain_sign(ctx, event_raw, signed_line, sizeof signed_line);
        if (i == 3) {
            /* 篡改 status 字段从 200 改为 500 */
            char* p = strstr(signed_line, "200");
            if (p != NULL) {
                p[0] = '5';
            }
        }
        fprintf(fp, "%s\n", signed_line);
    }
    fclose(fp);

    /* 验证应在第 3 行检测到篡改 */
    uint64_t verified = 0, broken = 0;
    char     errmsg[256] = {0};
    int vrc = audit_hash_chain_verify_file(ctx, tmppath, &verified, &broken, errmsg, sizeof errmsg);
    TEST_ASSERT(vrc == -1, "Verification must fail on tampered file");
    TEST_ASSERT(broken == 3, "Broken seq must be 3, got %lu", (unsigned long)broken);

    audit_hash_chain_destroy(ctx);
    unlink(tmppath);
}

TEST_CASE(test_audit_hash_chain_deletion_detection)
{
    const char*             secret = "test_audit_chain_secret_2026";
    audit_hash_chain_ctx_t* ctx = audit_hash_chain_create(secret);
    TEST_ASSERT(ctx != NULL, "audit_hash_chain_create failed");

    char tmppath[] = "/tmp/aigate_test_delete_XXXXXX";
    int  fd = mkstemp(tmppath);
    TEST_ASSERT(fd >= 0, "mkstemp failed");
    FILE* fp = fdopen(fd, "w");
    TEST_ASSERT(fp != NULL, "fdopen failed");

    /* 写入 5 条，但跳过写入第 3 条 (模拟删除行) */
    for (int i = 1; i <= 5; i++) {
        char event_raw[256];
        snprintf(event_raw, sizeof event_raw, "{\"trace_id\":\"tr_%d\",\"status\":200}", i);
        char signed_line[512];
        audit_hash_chain_sign(ctx, event_raw, signed_line, sizeof signed_line);
        if (i != 3) {
            fprintf(fp, "%s\n", signed_line);
        }
    }
    fclose(fp);

    /* 验证应在第 3 处（读到 seq 4 时发现 seq/prev_hash 断裂）报错 */
    uint64_t verified = 0, broken = 0;
    char     errmsg[256] = {0};
    int vrc = audit_hash_chain_verify_file(ctx, tmppath, &verified, &broken, errmsg, sizeof errmsg);
    TEST_ASSERT(vrc == -1, "Verification must fail when a line is deleted");
    TEST_ASSERT(broken > 0, "Broken seq must be > 0");

    audit_hash_chain_destroy(ctx);
    unlink(tmppath);
}
