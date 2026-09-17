/** @file upstream_client.c
 *  @brief libcurl non-streaming upstream transport (see upstream_client.h). */
#include "upstream_client.h"
#include "aigate_log.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void curl_init_once(void)
{
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

struct resp_buf {
  char *data;
  size_t len;
  size_t cap;
};

/* libcurl write callback: data first, userdata last. */
static size_t append_body(char *buf, size_t size, size_t nmemb, void *ud)
{
  struct resp_buf *rb = ud;
  size_t total = size * nmemb;
  if (rb->len + total + 1 > rb->cap) {
    size_t ncap = rb->cap ? rb->cap : 1024;
    while (rb->len + total + 1 > ncap)
      ncap *= 2;
    char *nd = realloc(rb->data, ncap);
    if (nd == NULL)
      return 0; /* abort transfer */
    rb->data = nd;
    rb->cap = ncap;
  }
  memcpy(rb->data + rb->len, buf, total);
  rb->len += total;
  rb->data[rb->len] = '\0';
  return total;
}

static int build_url(const model_rec_t *route, const char *path, char *out,
                     size_t cap)
{
  int n = snprintf(out, cap, "%s%s", route->endpoint, path);
  if (n < 0 || (size_t)n >= cap)
    return -1;
  return 0;
}

int upstream_call(const model_rec_t *route, const char *path,
                  const char *body_json, size_t body_len, long timeout_ms,
                  int *out_status, char **out_body, size_t *out_body_len)
{
  char url[1024];
  if (build_url(route, path, url, sizeof url) != 0)
    return -502;

  struct resp_buf rb = {0};
  struct curl_slist *hdrs = NULL;
  int rc = -502;
  long http_code = 0;

  pthread_once(&g_curl_once, curl_init_once);
  CURL *c = curl_easy_init();
  if (c == NULL)
    goto done;

  if (route->upstream_key[0] != '\0') {
    char auth[1080];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s",
             route->upstream_key);
    hdrs = curl_slist_append(hdrs, auth);
  }
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Accept: application/json");

  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_json);
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body_len);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &rb);
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 60000L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

  CURLcode cret = curl_easy_perform(c);
  if (cret == CURLE_OK) {
    if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code) == CURLE_OK) {
      *out_status = (int)http_code;
      *out_body = rb.data ? rb.data : (char *)"";
      *out_body_len = rb.len;
      rc = 0;
    }
  } else if (cret == CURLE_OPERATION_TIMEDOUT) {
    rc = -110;
  } else {
    AIGATE_LOG_WARN("upstream transport error: %s",
                    curl_easy_strerror(cret));
    rc = -502;
  }

done:
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  if (rc != 0 && out_body != NULL)
    *out_body = NULL;
  if (rc != 0 && out_body_len != NULL)
    *out_body_len = 0;
  return rc;
}
