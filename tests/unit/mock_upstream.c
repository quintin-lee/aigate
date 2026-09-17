/** @file mock_upstream.c
 *  @brief In-process blocking-socket mock upstream for upstream_client tests.
 *
 *  A single pthread accepts connections on a random 127.0.0.1 port and
 *  answers a minimal HTTP/1.1 POST contract:
 *    /chat → 200 OpenAI-shaped body with usage {7,11}
 *    /fail → 500 error body
 *    /slow → sleeps 2s then 200 (for timeout tests)
 *  The server reads the request (headers + body per Content-Length) then
 *  replies; one connection is handled at a time, which is fine for the
 *  sequential test cases.
 */
#include "mock_upstream.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

char g_mock_base[64] = {0};
int g_mock_port = 0;
int g_mock_listen_fd = -1;

static void *server_thread(void *arg)
{
  int lfd = g_mock_listen_fd;
  for (;;) {
    struct sockaddr_in cli;
    socklen_t clilen = sizeof cli;
    int cfd = accept(lfd, (struct sockaddr *)&cli, &clilen);
    if (cfd < 0)
      continue;

    /* read request until headers complete */
    char buf[65536];
    int total = 0;
    int content_length = 0;
    int hdr_done = 0;
    while (!hdr_done) {
      int r = read(cfd, buf + total, sizeof buf - 1 - total);
      if (r <= 0)
        break;
      total += r;
      buf[total] = '\0';
      const char *hrc = strstr(buf, "\r\n\r\n");
      if (hrc != NULL) {
        hdr_done = 1;
        /* parse Content-Length from headers */
        char *cl = strstr(buf, "Content-Length:");
        if (cl != NULL)
          content_length = atoi(cl + 15);
      }
    }
    /* read any remaining body bytes not yet received */
    while (total < content_length && total < (int)sizeof buf - 1) {
      int r = read(cfd, buf + total, sizeof buf - 1 - total);
      if (r <= 0)
        break;
      total += r;
      buf[total] = '\0';
    }

    char path[256] = "/";
    char *sp1 = strchr(buf, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (sp1 && sp2) {
      int plen = sp2 - sp1 - 1;
      if (plen > (int)sizeof path - 1)
        plen = sizeof path - 1;
      memcpy(path, sp1 + 1, (size_t)plen);
      path[plen] = '\0';
    }

    if (strcmp(path, "/slow") == 0) {
      struct timespec ts = {2, 0};
      nanosleep(&ts, NULL);
      const char *body = "{}";
      char resp[512];
      int blen = snprintf(resp, sizeof resp,
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                          (int)strlen(body), body);
      write(cfd, resp, (size_t)blen);
    } else if (strcmp(path, "/fail") == 0) {
      const char *body = "{\"error\":{\"message\":\"boom\"}}";
      char resp[512];
      int blen = snprintf(resp, sizeof resp,
                          "HTTP/1.1 500 Internal Server Error\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                          (int)strlen(body), body);
      write(cfd, resp, (size_t)blen);
    } else { /* /chat */
      const char *body =
          "{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion\","
          "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
          "\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
          "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":11,"
          "\"total_tokens\":18}}";
      char resp[2048];
      int blen = snprintf(resp, sizeof resp,
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                          (int)strlen(body), body);
      write(cfd, resp, (size_t)blen);
    }
    close(cfd);
  }
  return NULL;
}

const char *mock_upstream_start(void)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return NULL;
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  addr.sin_port = 0; /* OS picks */
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return NULL;
  }
  socklen_t alen = sizeof addr;
  getsockname(fd, (struct sockaddr *)&addr, &alen);
  g_mock_port = ntohs(addr.sin_port);
  if (listen(fd, 16) != 0) {
    close(fd);
    return NULL;
  }
  g_mock_listen_fd = fd;
  pthread_t th;
  if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
    close(fd);
    g_mock_listen_fd = -1;
    return NULL;
  }
  pthread_detach(th);
  snprintf(g_mock_base, sizeof g_mock_base, "http://127.0.0.1:%d", g_mock_port);
  return g_mock_base;
}

void mock_upstream_stop(void)
{
  if (g_mock_listen_fd >= 0) {
    close(g_mock_listen_fd);
    g_mock_listen_fd = -1;
  }
}
