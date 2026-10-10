/** @file test_ip_ban_table.c
 *  @brief Unit tests for in-memory concurrent IP ban table.
 */
#include "policy/ip_ban_table.h"
#include "run_tests.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

TEST_CASE(test_ip_ban_table_lifecycle)
{
    ip_ban_table_t* tbl = ip_ban_table_create();
    TEST_ASSERT(tbl != NULL, "tbl created");

    char reason[64] = {0};
    TEST_ASSERT(!ip_ban_table_is_banned(tbl, "192.168.1.50", reason, sizeof(reason)),
                "initially not banned");

    int rc = ip_ban_table_ban(tbl, "192.168.1.50", 10, "canary_compromised");
    TEST_ASSERT(rc == 0, "ban succeeded");

    TEST_ASSERT(ip_ban_table_is_banned(tbl, "192.168.1.50", reason, sizeof(reason)), "now banned");
    TEST_ASSERT(strcmp(reason, "canary_compromised") == 0, "reason matches");

    /* Second check to verify hit count increment */
    TEST_ASSERT(ip_ban_table_is_banned(tbl, "192.168.1.50", reason, sizeof(reason)),
                "still banned");

    ip_ban_info_t list[10];
    size_t        n = ip_ban_table_list(tbl, list, 10);
    TEST_ASSERT(n == 1, "list has 1 entry");
    TEST_ASSERT(strcmp(list[0].ip, "192.168.1.50") == 0, "list ip matches");
    TEST_ASSERT(list[0].hit_count >= 2, "hit_count >= 2");

    rc = ip_ban_table_unban(tbl, "192.168.1.50");
    TEST_ASSERT(rc == 0, "unban succeeded");
    TEST_ASSERT(!ip_ban_table_is_banned(tbl, "192.168.1.50", reason, sizeof(reason)),
                "unbanned");

    /* Unban non-existent returns -1 */
    TEST_ASSERT(ip_ban_table_unban(tbl, "192.168.1.50") == -1, "unban non-existent fails");

    ip_ban_table_destroy(tbl);
}

TEST_CASE(test_ip_ban_table_expiration)
{
    ip_ban_table_t* tbl = ip_ban_table_create();
    TEST_ASSERT(tbl != NULL, "tbl created");

    /* Ban with negative TTL (already expired) */
    int rc = ip_ban_table_ban(tbl, "10.0.0.1", -5, "quick_expire");
    TEST_ASSERT(rc == 0, "ban succeeded");

    /* When TTL is <= 0, ip_ban_table_ban defaults to 3600; let's ban with 1s and sleep */
    rc = ip_ban_table_ban(tbl, "10.0.0.2", 1, "one_sec_expire");
    TEST_ASSERT(rc == 0, "ban with 1s succeeded");

    char reason[64] = {0};
    TEST_ASSERT(ip_ban_table_is_banned(tbl, "10.0.0.2", reason, sizeof(reason)),
                "initially banned");

    /* Wait 1.1s for expiration */
    usleep(1100000);

    TEST_ASSERT(!ip_ban_table_is_banned(tbl, "10.0.0.2", reason, sizeof(reason)),
                "expired ban is no longer banned");

    ip_ban_info_t list[10];
    size_t        n = ip_ban_table_list(tbl, list, 10);
    /* 10.0.0.1 (default 3600s) should be in list, 10.0.0.2 should not */
    bool found_expired = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].ip, "10.0.0.2") == 0) {
            found_expired = true;
        }
    }
    TEST_ASSERT(!found_expired, "expired entry not in active list");

    ip_ban_table_destroy(tbl);
}

struct bench_arg {
    ip_ban_table_t* tbl;
    int             thread_id;
};

static void*
worker_thread(void* arg)
{
    struct bench_arg* ba = (struct bench_arg*)arg;
    char              ip[48];
    snprintf(ip, sizeof(ip), "172.16.0.%d", ba->thread_id);

    ip_ban_table_ban(ba->tbl, ip, 100, "concurrency_test");

    char reason[64];
    for (int i = 0; i < 50; i++) {
        ip_ban_table_is_banned(ba->tbl, ip, reason, sizeof(reason));
    }
    return NULL;
}

TEST_CASE(test_ip_ban_table_concurrency)
{
    ip_ban_table_t* tbl = ip_ban_table_create();
    TEST_ASSERT(tbl != NULL, "tbl created");

    pthread_t        th[8];
    struct bench_arg args[8];

    for (int i = 0; i < 8; i++) {
        args[i].tbl = tbl;
        args[i].thread_id = i + 1;
        pthread_create(&th[i], NULL, worker_thread, &args[i]);
    }

    for (int i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
    }

    ip_ban_info_t list[32];
    size_t        n = ip_ban_table_list(tbl, list, 32);
    TEST_ASSERT(n == 8, "8 IPs banned concurrently (got %zu)", n);

    ip_ban_table_destroy(tbl);
}
