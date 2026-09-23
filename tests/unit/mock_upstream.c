/** @file mock_upstream.c
 *  @brief In-process blocking-socket mock upstream (see mock_upstream.h).
 *
 *  A single joinable pthread accepts connections on a random 127.0.0.1
 *  port (poll-based so stop is prompt) and answers a minimal HTTP/1.1
 *  POST contract:
 *    /chat, /chat/completions → 200 OpenAI-shaped body, usage {7, 11}
 *    (or 500 when mock_upstream_fail_all is enabled)
 *    /fail                     → 500 error body
 *    /slow                     → sleeps 2s then 200 {} (timeout tests)
 *  The last request's path + body are recorded for assertions.
 */
#include "mock_upstream.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_BODY 4096

struct mock_upstream {
    int             listen_fd;
    int             port;
    char            base[64];
    pthread_t       thread;
    int             running;
    int             fail_all;
    int             mock_status; /* forced status (100..599) for all requests */
    int             request_count;
    char            last_path[256];
    char            last_body[MAX_BODY];
    pthread_mutex_t mtx; /* guards recorded fields + flag reads */
};

static void*
server_thread(void* arg)
{
    mock_upstream_t* mu = arg;
    for (;;) {
        struct pollfd pfd;
        pfd.fd = mu->listen_fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 100) <= 0) {
            continue;
        }
        if (!mu->running) {
            break;
        }

        struct sockaddr_in cli;
        socklen_t          clilen = sizeof cli;
        int                cfd = accept(mu->listen_fd, (struct sockaddr*)&cli, &clilen);
        if (cfd < 0) {
            continue;
        }

        /* read headers, then body (body may already be partially in the buffer) */
        char buf[MAX_BODY];
        int  total = 0;
        int  content_length = 0;
        int  hdr_off = -1; /* offset of first body byte, -1 until known */
        for (;;) {
            int r = read(cfd, buf + total, sizeof buf - 1 - total);
            if (r <= 0) {
                break;
            }
            total += r;
            buf[total] = '\0';
            if (hdr_off < 0) {
                const char* hrc = strstr(buf, "\r\n\r\n");
                if (hrc != NULL) {
                    hdr_off = (int)(hrc - buf) + 4;
                    char* cl = strstr(buf, "Content-Length:");
                    if (cl != NULL) {
                        content_length = atoi(cl + 15);
                    }
                    if (content_length <= 0) {
                        break; /* no body expected */
                    }
                }
            }
            if (hdr_off >= 0 && total >= hdr_off + content_length) {
                break; /* full request received */
            }
        }

        char  path[256] = "/";
        char* sp1 = strchr(buf, ' ');
        char* sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        if (sp1 && sp2) {
            int plen = sp2 - sp1 - 1;
            if (plen > (int)sizeof path - 1) {
                plen = sizeof path - 1;
            }
            memcpy(path, sp1 + 1, (size_t)plen);
            path[plen] = '\0';
        }

        /* record the request for test assertions */
        pthread_mutex_lock(&mu->mtx);
        mu->request_count++;
        snprintf(mu->last_path, sizeof mu->last_path, "%s", path);
        const char* hstart = strstr(buf, "\r\n\r\n");
        size_t      body_off = hstart != NULL ? (size_t)(hstart - buf + 4) : 0;
        snprintf(mu->last_body, sizeof mu->last_body, "%s", buf + body_off);
        int fail = mu->fail_all;
        int mstatus = mu->mock_status;
        pthread_mutex_unlock(&mu->mtx);

        int is_fail = 0;
        if (strcmp(path, "/fail") == 0 || fail) {
            is_fail = 1;
        }

        int is_streaming_req = (strstr(mu->last_body, "\"stream\":true") != NULL ||
                                strstr(mu->last_body, "\"stream\": true") != NULL);
        int is_slow = (strstr(mu->last_body, "stream-slow") != NULL);

        if (mstatus >= 100 && mstatus <= 599) {
            const char* body = "{\"mock\":\"forced-status\"}";
            char resp[512];
            int blen = snprintf(resp,
                                sizeof resp,
                                "HTTP/1.1 %d Mock\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                mstatus,
                                strlen(body),
                                body);
            write(cfd, resp, (size_t)blen);
            close(cfd);
            continue;
        }
        if (is_fail) {
            int         status = (fail >= 400 && fail <= 599) ? fail : 500;
            const char* status_text =
                (status == 429) ? "Too Many Requests" : "Internal Server Error";
            const char* body = (status == 429) ? "{\"error\":{\"message\":\"rate limited\"}}"
                                               : "{\"error\":{\"message\":\"boom\"}}";
            char        resp[512];
            int         blen = snprintf(resp,
                                        sizeof resp,
                                        "HTTP/1.1 %d %s\r\n"
                                        "Content-Type: application/json\r\n"
                                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                                        status,
                                        status_text,
                                        (int)strlen(body),
                                        body);
            write(cfd, resp, (size_t)blen);
        } else if (strcmp(path, "/slow") == 0) {
            struct timespec ts = {2, 0};
            nanosleep(&ts, NULL);
            const char* body = "{}";
            char        resp[512];
            int         blen = snprintf(resp,
                                        sizeof resp,
                                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                                        (int)strlen(body),
                                        body);
            write(cfd, resp, (size_t)blen);
        } else if (strcmp(path, "/v1/messages") == 0 || strcmp(path, "/messages") == 0) {
            if (is_streaming_req) {
                const char* hdr = "HTTP/1.1 200 OK\r\nContent-Type: "
                                  "text/event-stream\r\nConnection: close\r\n\r\n";
                write(cfd, hdr, strlen(hdr));
                const char* c1 =
                    "event: message_start\r\ndata: "
                    "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_mock_stream\",\"type\":"
                    "\"message\",\"role\":\"assistant\",\"model\":\"claude-3-5-sonnet-20241022\","
                    "\"usage\":{\"input_tokens\":12,\"output_tokens\":1}}}\r\n\r\n";
                write(cfd, c1, strlen(c1));
                struct timespec sl = {0, 10 * 1000000};
                nanosleep(&sl, NULL);
                const char* c2 = "event: content_block_delta\r\ndata: "
                                 "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                 "\"type\":\"text_delta\",\"text\":\"Hello \"}}\r\n\r\n";
                write(cfd, c2, strlen(c2));
                nanosleep(&sl, NULL);
                const char* c3 = "event: content_block_delta\r\ndata: "
                                 "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                 "\"type\":\"text_delta\",\"text\":\"from Claude\"}}\r\n\r\n";
                write(cfd, c3, strlen(c3));
                nanosleep(&sl, NULL);
                const char* c4 = "event: message_delta\r\ndata: "
                                 "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_"
                                 "turn\"},\"usage\":{\"output_tokens\":18}}\r\n\r\n";
                write(cfd, c4, strlen(c4));
                nanosleep(&sl, NULL);
                const char* c5 = "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n";
                write(cfd, c5, strlen(c5));
            } else {
                const char* body =
                    "{\"id\":\"msg_mock_123\",\"type\":\"message\",\"role\":\"assistant\","
                    "\"model\":\"claude-3-5-sonnet-20241022\",\"content\":[{\"type\":\"text\","
                    "\"text\":\"Hello from Claude non-stream\"}],\"stop_reason\":\"end_turn\","
                    "\"usage\":{\"input_tokens\":12,\"output_tokens\":18}}";
                char resp[2048];
                int  blen = snprintf(resp,
                                     sizeof resp,
                                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                     "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                                     (int)strlen(body),
                                     body);
                write(cfd, resp, (size_t)blen);
            }
        } else if (strcmp(path, "/mock/stream-slow") == 0 || (is_streaming_req && is_slow)) {
            const char* hdr =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
            write(cfd, hdr, strlen(hdr));
            const char* c1 =
                "data: {\"id\":\"1\",\"choices\":[{\"delta\":{\"content\":\"start\"}}]}\n\n";
            write(cfd, c1, strlen(c1));
            struct timespec sl = {1, 200 * 1000000}; /* 1200ms */
            nanosleep(&sl, NULL);
            const char* c2 = "data: [DONE]\n\n";
            write(cfd, c2, strlen(c2));
        } else if (strcmp(path, "/mock/stream") == 0 || is_streaming_req) {
            const char* hdr =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
            write(cfd, hdr, strlen(hdr));
            const char* c1 =
                "data: {\"id\":\"1\",\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n";
            write(cfd, c1, strlen(c1));
            struct timespec sl = {0, 10 * 1000000}; /* 10ms */
            nanosleep(&sl, NULL);
            const char* c2 =
                "data: {\"id\":\"1\",\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n";
            write(cfd, c2, strlen(c2));
            nanosleep(&sl, NULL);
            const char* c3 =
                "data: "
                "{\"choices\":[],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":7}}\n\n";
            write(cfd, c3, strlen(c3));
            nanosleep(&sl, NULL);
            const char* c4 = "data: [DONE]\n\n";
            write(cfd, c4, strlen(c4));
        } else if (strcmp(path, "/v1/embeddings") == 0 || strcmp(path, "/embeddings") == 0) {
            const char* body = "{\"object\":\"list\",\"data\":[{\"object\":\"embedding\",\"index\":"
                               "0,\"embedding\":[0.1,0.2,0.3]}],\"model\":\"text-embedding-3-"
                               "small\",\"usage\":{\"prompt_tokens\":8,\"total_tokens\":8}}";
            char        resp[2048];
            int         blen = snprintf(resp,
                                        sizeof resp,
                                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                                        (int)strlen(body),
                                        body);
            write(cfd, resp, (size_t)blen);
        } else if (strstr(path, ":embedContent") != NULL) {
            const char* body = "{\"embedding\":{\"values\":[0.05,0.15,0.25]},\"usageMetadata\":{"
                               "\"promptTokenCount\":6}}";
            char        resp[2048];
            int         blen = snprintf(resp,
                                        sizeof resp,
                                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                                        (int)strlen(body),
                                        body);
            write(cfd, resp, (size_t)blen);
        } else if (strstr(path, ":batchEmbedContents") != NULL) {
            const char* body = "{\"embeddings\":[{\"values\":[0.05,0.15]},{\"values\":[0.25,0.35]}]"
                               ",\"usageMetadata\":{\"promptTokenCount\":12}}";
            char        resp[2048];
            int         blen = snprintf(resp,
                                        sizeof resp,
                                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                                        (int)strlen(body),
                                        body);
            write(cfd, resp, (size_t)blen);
        } else if (strstr(path, ":generateContent") != NULL && strstr(path, "alt=sse") == NULL) {
            const char* body = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello from "
                               "Gemini\"}],\"role\":\"model\"},\"finishReason\":\"STOP\",\"index\":"
                               "0}],\"usageMetadata\":{\"promptTokenCount\":9,"
                               "\"candidatesTokenCount\":5,\"totalTokenCount\":14}}";
            char        resp[2048];
            int         blen = snprintf(resp,
                                        sizeof resp,
                                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                                        (int)strlen(body),
                                        body);
            write(cfd, resp, (size_t)blen);
        } else if (strstr(path, ":streamGenerateContent") != NULL ||
                   strstr(path, "alt=sse") != NULL) {
            const char* hdr =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
            write(cfd, hdr, strlen(hdr));
            const char* c1 = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello "
                             "\"}],\"role\":\"model\"},\"index\":0}]}\n\n";
            write(cfd, c1, strlen(c1));
            struct timespec sl = {0, 10 * 1000000};
            nanosleep(&sl, NULL);
            const char* c2 =
                "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"from Gemini "
                "SSE\"}],\"role\":\"model\"},\"finishReason\":\"STOP\",\"index\":0}],"
                "\"usageMetadata\":{\"promptTokenCount\":10,\"candidatesTokenCount\":6,"
                "\"totalTokenCount\":16}}\n\n";
            write(cfd, c2, strlen(c2));
        } else { /* /chat, /chat/completions, default */
            const char* body = "{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion\","
                               "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
                               "\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
                               "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":11,"
                               "\"total_tokens\":18}}";
            char        resp[2048];
            int         blen = snprintf(resp,
                                        sizeof resp,
                                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                                        (int)strlen(body),
                                        body);
            write(cfd, resp, (size_t)blen);
        }
        close(cfd);
    }
    return NULL;
}

mock_upstream_t*
mock_upstream_start(void)
{
    mock_upstream_t* mu = calloc(1, sizeof *mu);
    if (mu == NULL) {
        return NULL;
    }
    pthread_mutex_init(&mu->mtx, NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        free(mu);
        return NULL;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = 0; /* OS picks */
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
        close(fd);
        free(mu);
        return NULL;
    }
    socklen_t alen = sizeof addr;
    getsockname(fd, (struct sockaddr*)&addr, &alen);
    mu->port = ntohs(addr.sin_port);
    if (listen(fd, 16) != 0) {
        close(fd);
        free(mu);
        return NULL;
    }
    mu->listen_fd = fd;
    mu->running = 1;
    if (pthread_create(&mu->thread, NULL, server_thread, mu) != 0) {
        close(fd);
        free(mu);
        return NULL;
    }
    snprintf(mu->base, sizeof mu->base, "http://127.0.0.1:%d", mu->port);
    return mu;
}

void
mock_upstream_stop(mock_upstream_t* mu)
{
    if (mu == NULL) {
        return;
    }
    mu->running = 0;
    close(mu->listen_fd);
    pthread_join(mu->thread, NULL);
    free(mu);
}

const char*
mock_upstream_base(const mock_upstream_t* mu)
{
    return mu->base;
}

void
mock_upstream_fail_all(mock_upstream_t* mu, int fail)
{
    pthread_mutex_lock(&mu->mtx);
    mu->fail_all = fail;
    pthread_mutex_unlock(&mu->mtx);
}
void
mock_upstream_status(mock_upstream_t* mu, int status)
{
    pthread_mutex_lock(&mu->mtx);
    mu->mock_status = status;
    pthread_mutex_unlock(&mu->mtx);
}

int
mock_upstream_request_count(const mock_upstream_t* mu)
{
    return mu->request_count;
}

const char*
mock_upstream_last_body(const mock_upstream_t* mu)
{
    return mu->last_body;
}

const char*
mock_upstream_last_path(const mock_upstream_t* mu)
{
    return mu->last_path;
}
