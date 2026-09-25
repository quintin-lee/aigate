/** @file redis_client.c
 *  @brief hiredis connection helpers and Lua script evaluation. */
#include "redis_client.h"
#include "aigate_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static int
parse_redis_url(
    const char* url, char* host, size_t host_cap, int* port, char* pass, size_t pass_cap, int* db)
{
    const char* p = url;
    if (strncmp(p, "redis://", 8) == 0) {
        p += 8;
    }

    host[0] = '\0';
    pass[0] = '\0';
    *port = 6379;
    *db = 0;

    /* Check for password: [:password@] or user:password@ */
    const char* at = strchr(p, '@');
    if (at != NULL) {
        size_t auth_len = (size_t)(at - p);
        char   auth_buf[256];
        if (auth_len >= sizeof(auth_buf)) {
            auth_len = sizeof(auth_buf) - 1;
        }
        memcpy(auth_buf, p, auth_len);
        auth_buf[auth_len] = '\0';

        char* colon = strchr(auth_buf, ':');
        if (colon != NULL) {
            colon++;
            size_t l = strlen(colon);
            if (l >= pass_cap) {
                l = pass_cap - 1;
            }
            memcpy(pass, colon, l);
            pass[l] = '\0';
        } else {
            size_t l = strlen(auth_buf);
            if (l >= pass_cap) {
                l = pass_cap - 1;
            }
            memcpy(pass, auth_buf, l);
            pass[l] = '\0';
        }
        p = at + 1;
    }

    /* Host and port */
    const char* slash = strchr(p, '/');
    char        hostport[256];
    size_t      hp_len = slash ? (size_t)(slash - p) : strlen(p);
    if (hp_len >= sizeof(hostport)) {
        hp_len = sizeof(hostport) - 1;
    }
    memcpy(hostport, p, hp_len);
    hostport[hp_len] = '\0';

    char* colon = strchr(hostport, ':');
    if (colon != NULL) {
        *colon = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) {
            *port = 6379;
        }
    }
    const char* target_host = hostport[0] ? hostport : "127.0.0.1";
    size_t      hl = strlen(target_host);
    if (hl >= host_cap) {
        hl = host_cap - 1;
    }
    memcpy(host, target_host, hl);
    host[hl] = '\0';

    /* Database index */
    if (slash != NULL && slash[1] != '\0') {
        *db = atoi(slash + 1);
        if (*db < 0) {
            *db = 0;
        }
    }

    return 0;
}

redisContext*
redis_connect_url(const char* url, int timeout_ms)
{
    if (url == NULL || *url == '\0') {
        return NULL;
    }

    char host[256];
    char pass[128];
    int  port = 6379;
    int  db = 0;

    parse_redis_url(url, host, sizeof(host), &port, pass, sizeof(pass), &db);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (tv.tv_sec == 0 && tv.tv_usec < 1000) {
        tv.tv_usec = 1000;
    }

    redisContext* c = redisConnectWithTimeout(host, port, tv);
    if (c == NULL || c->err) {
        if (c != NULL) {
            AIGATE_LOG_WARN("redis connect failed to %s:%d: %s", host, port, c->errstr);
            redisFree(c);
        }
        return NULL;
    }

    redisSetTimeout(c, tv);

    if (pass[0] != '\0') {
        redisReply* reply = (redisReply*)redisCommand(c, "AUTH %s", pass);
        if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
            AIGATE_LOG_ERROR(
                "redis AUTH failed on %s:%d: %s", host, port, reply ? reply->str : "null reply");
            if (reply) {
                freeReplyObject(reply);
            }
            redisFree(c);
            return NULL;
        }
        freeReplyObject(reply);
    }

    if (db > 0) {
        redisReply* reply = (redisReply*)redisCommand(c, "SELECT %d", db);
        if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
            AIGATE_LOG_ERROR("redis SELECT %d failed on %s:%d: %s",
                             db,
                             host,
                             port,
                             reply ? reply->str : "null reply");
            if (reply) {
                freeReplyObject(reply);
            }
            redisFree(c);
            return NULL;
        }
        freeReplyObject(reply);
    }

    return c;
}

int
redis_script_load(redisContext* c, const char* script, char* sha_out)
{
    if (c == NULL || script == NULL || sha_out == NULL) {
        return -1;
    }

    redisReply* reply = (redisReply*)redisCommand(c, "SCRIPT LOAD %s", script);
    if (reply == NULL || reply->type != REDIS_REPLY_STRING || reply->len != 40) {
        AIGATE_LOG_ERROR("redis SCRIPT LOAD failed: %s", reply ? reply->str : "network error");
        if (reply) {
            freeReplyObject(reply);
        }
        return -1;
    }

    memcpy(sha_out, reply->str, 40);
    sha_out[40] = '\0';
    freeReplyObject(reply);
    return 0;
}

redisReply*
redis_eval_sha(redisContext* c,
               const char*   sha,
               const char*   script,
               int           numkeys,
               const char**  keys,
               const char**  argv,
               int           argc)
{
    if (c == NULL) {
        return NULL;
    }

    /* Build command vector: EVALSHA sha numkeys [keys...] [argv...] */
    int          total_args = 3 + numkeys + argc;
    const char** cmd_argv = malloc((size_t)total_args * sizeof(char*));
    size_t*      cmd_lens = malloc((size_t)total_args * sizeof(size_t));
    if (!cmd_argv || !cmd_lens) {
        free(cmd_argv);
        free(cmd_lens);
        return NULL;
    }

    char numkeys_buf[16];
    snprintf(numkeys_buf, sizeof(numkeys_buf), "%d", numkeys);

    cmd_argv[0] = "EVALSHA";
    cmd_lens[0] = 7;
    cmd_argv[1] = sha;
    cmd_lens[1] = strlen(sha);
    cmd_argv[2] = numkeys_buf;
    cmd_lens[2] = strlen(numkeys_buf);

    int idx = 3;
    for (int i = 0; i < numkeys; i++) {
        cmd_argv[idx] = keys[i];
        cmd_lens[idx] = strlen(keys[i]);
        idx++;
    }
    for (int i = 0; i < argc; i++) {
        cmd_argv[idx] = argv[i];
        cmd_lens[idx] = strlen(argv[i]);
        idx++;
    }

    redisReply* reply = (redisReply*)redisCommandArgv(c, total_args, cmd_argv, cmd_lens);

    /* Check if NOSCRIPT error happened (e.g. Redis restarted or FLUSHDB was called) */
    if (reply != NULL && reply->type == REDIS_REPLY_ERROR &&
        strncmp(reply->str, "NOSCRIPT", 8) == 0) {
        freeReplyObject(reply);
        reply = NULL;

        /* Reload script */
        char reloaded_sha[48];
        if (redis_script_load(c, script, reloaded_sha) == 0) {
            cmd_argv[1] = reloaded_sha;
            cmd_lens[1] = strlen(reloaded_sha);
            reply = (redisReply*)redisCommandArgv(c, total_args, cmd_argv, cmd_lens);
        }
    }

    free(cmd_argv);
    free(cmd_lens);
    return reply;
}
