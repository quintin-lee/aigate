/** @file main.c
 *  @brief aigate process entry point (spec §5, Task 11).
 *
 *  Boot sequence:
 *    1. Config loading from environment variables.
 *    2. PostgreSQL store initialization & idempotent schema migration.
 *    3. Upstream master key decoding (AES-256-GCM).
 *    4. Pipeline core initialization (auth, rate limiting, router, usage meter).
 *    5. CivetWeb HTTP transport initialization.
 *    6. Signal installation (SIGINT / SIGTERM) & main loop.
 *    7. Graceful shutdown: stop transport, flush usage meter, close store.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "aigate_core.h"
#include "aigate_log.h"
#include "config.h"
#include "pg_store.h"
#include "secrets.h"
#include "transport_civetweb.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void
sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

int
main(void)
{
    /* 1. Config loading */
    aigate_config cfg;
    if (aigate_config_load(&cfg) != 0) {
        AIGATE_LOG_ERROR("main: config load failed, exiting");
        return 1;
    }

    /* 2. Database connection & schema migration */
    pg_store_t* ps = pg_store_open(cfg.pg_dsn, NULL);
    if (ps == NULL) {
        AIGATE_LOG_ERROR("main: failed to open postgres store (%s)", cfg.pg_dsn);
        return 1;
    }
    if (pg_store_migrate(ps) != 0) {
        AIGATE_LOG_ERROR("main: postgres schema migration failed");
        pg_store_close(ps);
        return 1;
    }

    /* 3. Upstream master key */
    uint8_t        master[32];
    const uint8_t* master_ptr = NULL;
    if (cfg.master_key[0] != '\0') {
        if (hex_to_bytes32(cfg.master_key, master) != 0) {
            AIGATE_LOG_ERROR("main: invalid AIGATE_MASTER_KEY hex");
            pg_store_close(ps);
            return 1;
        }
        master_ptr = master;
    }

    /* 4. Core pipeline initialization */
    aigate_core core;
    if (aigate_core_init(&core, ps, master_ptr, cfg.upstream_timeout_ms, 5) != 0) {
        AIGATE_LOG_ERROR("main: failed to initialize aigate core pipeline");
        pg_store_close(ps);
        return 1;
    }

    /* 5. Start CivetWeb transport */
    transport_civetweb_t* cw = transport_civetweb_start(
        &core, ps, cfg.admin_token_hash, cfg.listen, cfg.metrics_acl);
    if (cw == NULL) {
        AIGATE_LOG_ERROR("main: failed to start HTTP transport on %s", cfg.listen);
        aigate_core_shutdown(&core);
        pg_store_close(ps);
        return 1;
    }

    /* 6. Signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    AIGATE_LOG_INFO("aigate ready on %s (pid: %d)", cfg.listen, (int)getpid());

    /* 7. Run until signal */
    while (!g_stop) {
        struct timespec ts = {0, 100 * 1000000}; /* 100ms */
        nanosleep(&ts, NULL);
    }

    /* 8. Graceful shutdown */
    AIGATE_LOG_INFO("aigate shutting down gracefully...");
    transport_civetweb_stop(cw);
    aigate_core_shutdown(&core);
    pg_store_close(ps);
    AIGATE_LOG_INFO("aigate shutdown complete");

    return 0;
}
