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
#include "admin_api.h"
#include "circuit_breaker.h"
#include "config.h"
#include "health_prober.h"
#include <openssl/evp.h>
#include "redis_pool.h"
#include "ratelimit.h"
#include "secrets.h"
#include "transport_civetweb.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** Shutdown flag: the SIGINT/SIGTERM handler sets it to 1; the main loop exits for graceful shutdown. */
static volatile sig_atomic_t g_stop = 0;
/** Reload flag: the SIGHUP handler sets it to 1; the main loop reloads dynamic configuration. */
static volatile sig_atomic_t g_reload = 0;

/** @brief Signal handler: set g_reload on SIGHUP, set g_stop on SIGINT/SIGTERM (async-signal-safe operations only). */
static void
sig_handler(int sig)
{
    if (sig == SIGHUP) {
        g_reload = 1;
        return;
    }
    g_stop = 1;
}

/** @brief Program entry: load config, init storage/routing/transport, then serve until signaled to exit. */
int
main(int argc, char** argv)
{
    /* 0. Offline utility commands */
    if (argc >= 2 && strcmp(argv[1], "--rotate-master-key") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                    "Usage: %s --rotate-master-key <old_master_hex_64> <new_master_hex_64>\n",
                    argv[0]);
            return 1;
        }
        uint8_t old_m[32], new_m[32];
        if (hex_to_bytes32(argv[2], old_m) != 0) {
            fprintf(stderr, "Error: invalid old master key (expected 64 hex characters)\n");
            return 1;
        }
        if (hex_to_bytes32(argv[3], new_m) != 0) {
            fprintf(stderr, "Error: invalid new master key (expected 64 hex characters)\n");
            OPENSSL_cleanse(old_m, sizeof old_m);
            return 1;
        }

        aigate_config cfg;
        if (aigate_config_load(&cfg) != 0) {
            fprintf(stderr, "Error: failed to load database configuration\n");
            OPENSSL_cleanse(old_m, sizeof old_m);
            OPENSSL_cleanse(new_m, sizeof new_m);
            return 1;
        }

        pg_store_t* ps = pg_store_open(cfg.pg_dsn, NULL);
        if (ps == NULL) {
            fprintf(stderr, "Error: failed to connect to database (%s)\n", cfg.pg_dsn);
            OPENSSL_cleanse(old_m, sizeof old_m);
            OPENSSL_cleanse(new_m, sizeof new_m);
            return 1;
        }

        int rotated_count = 0;
        if (pg_store_rotate_master_key(ps, old_m, new_m, &rotated_count) != 0) {
            fprintf(stderr, "Error: master key rotation failed (transaction rolled back)\n");
            pg_store_close(ps);
            OPENSSL_cleanse(old_m, sizeof old_m);
            OPENSSL_cleanse(new_m, sizeof new_m);
            return 1;
        }

        printf("Successfully rotated %d upstream model/provider keys to new master key\n",
               rotated_count);
        pg_store_close(ps);
        OPENSSL_cleanse(old_m, sizeof old_m);
        OPENSSL_cleanse(new_m, sizeof new_m);
        return 0;
    }

    /* 1. Config loading */
    aigate_config cfg;
    if (aigate_config_load(&cfg) != 0) {
        AIGATE_LOG_ERROR("main: config load failed, exiting");
        return 1;
    }
    aigate_log_init(cfg.log_format, cfg.log_level);

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
            OPENSSL_cleanse(master, sizeof master);
            pg_store_close(ps);
            return 1;
        }
        master_ptr = master;
    }

    /* 4. Core pipeline initialization */
    aigate_core core;
    if (aigate_core_init(&core, ps, master_ptr, cfg.upstream_timeout_ms, cfg.usage_flush_s) != 0) {
        AIGATE_LOG_ERROR("main: failed to initialize aigate core pipeline");
        /* core_init rolled back (and cleansed) the router's master copy */
        OPENSSL_cleanse(master, sizeof master);
        pg_store_close(ps);
        return 1;
    }

    if (core.hp != NULL) {
        health_prober_start(core.hp);
    }

    if (core.rl != NULL) {
        ratelimit_set_fail_open(core.rl, cfg.redis_fail_open);
    }
    if (core.cb != NULL) {
        cb_set_fail_open(core.cb, cfg.redis_fail_open);
    }

    /* 4b. Optional Redis clustering: inject shared pool into rate limiter,
     *     circuit breaker, and admin lockout subsystems. */
    redis_pool_t* redis_pool = NULL;
    if (cfg.redis_url[0] != '\0') {
        redis_pool = redis_pool_create(cfg.redis_url, cfg.redis_pool_size, cfg.redis_timeout_ms);
        if (redis_pool != NULL) {
            ratelimit_set_redis_pool(core.rl, redis_pool);
            cb_set_redis_pool(core.cb, redis_pool);
            admin_lockout_set_pool(redis_pool);
            AIGATE_LOG_INFO("main: Redis clustering enabled (%s, pool=%d, fail_open=%d)",
                            cfg.redis_url,
                            cfg.redis_pool_size,
                            cfg.redis_fail_open);
        } else {
            AIGATE_LOG_WARN("main: AIGATE_REDIS_URL set but Redis pool creation failed "
                            "— running in standalone mode");
        }
    }

    /* 5. Start CivetWeb transport */
    transport_civetweb_t* cw = transport_civetweb_start_tls(&core,
                                                            ps,
                                                            cfg.admin_token_hash,
                                                            cfg.listen,
                                                            cfg.metrics_acl,
                                                            cfg.max_body_bytes,
                                                            cfg.worker_threads,
                                                            cfg.request_timeout_ms,
                                                            cfg.trusted_proxies,
                                                            cfg.cors_allow_origin,
                                                            cfg.ssl_cert,
                                                            cfg.ssl_key);
    if (cw == NULL) {
        AIGATE_LOG_ERROR("main: failed to start HTTP transport on %s", cfg.listen);
        aigate_core_shutdown(&core);
        OPENSSL_cleanse(master, sizeof master);
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
    sigaction(SIGHUP, &sa, NULL);

    AIGATE_LOG_INFO("aigate ready on %s (pid: %d)", cfg.listen, (int)getpid());

    /* 7. Run until signal */
    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            aigate_config new_cfg;
            if (aigate_config_load(&new_cfg) == 0) {
                aigate_log_init(new_cfg.log_format, new_cfg.log_level);
                transport_civetweb_update_cors(cw, new_cfg.cors_allow_origin);
                transport_civetweb_update_trusted_proxies(cw, new_cfg.trusted_proxies);
                AIGATE_LOG_INFO(
                    "main: configuration reloaded via SIGHUP (level=%s, format=%s, cors=%s, "
                    "proxies=%s)",
                    new_cfg.log_level,
                    new_cfg.log_format,
                    new_cfg.cors_allow_origin,
                    new_cfg.trusted_proxies);
            } else {
                AIGATE_LOG_WARN("main: SIGHUP received but configuration reload failed");
            }
        }
        struct timespec ts = {0, 100 * 1000000}; /* 100ms */
        nanosleep(&ts, NULL);
    }

    /* 8. Graceful shutdown: draining phase first */
    if (cfg.drain_timeout_s > 0) {
        AIGATE_LOG_INFO("aigate received shutdown signal, draining traffic for %d seconds...",
                        cfg.drain_timeout_s);
        transport_civetweb_set_draining(cw, 1);
        for (int i = 0; i < cfg.drain_timeout_s * 10; i++) {
            struct timespec ts = {0, 100 * 1000000}; /* 100ms */
            nanosleep(&ts, NULL);
        }
    }

    AIGATE_LOG_INFO("aigate shutting down gracefully...");
    transport_civetweb_stop(cw);
    aigate_core_shutdown(&core);
    if (redis_pool != NULL) {
        admin_lockout_set_pool(NULL);
        ratelimit_set_redis_pool(core.rl, NULL);
        cb_set_redis_pool(core.cb, NULL);
        redis_pool_destroy(redis_pool);
    }
    pg_store_close(ps);
    AIGATE_LOG_INFO("aigate shutdown complete");
    OPENSSL_cleanse(master, sizeof master);

    return 0;
}
