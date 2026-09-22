/** @file test_upstream_streaming.c
 *  @brief Unit tests for upstream_stream_call and silence timeout (Plan 2, Task 1).
 */
#include "run_tests.h"
#include "mock_upstream.h"
#include "upstream_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct stream_capture {
    char   buf[8192];
    size_t len;
    int    chunks_count;
};

static int
capture_chunk(void* user_data, const void* chunk, size_t len)
{
    struct stream_capture* sc = user_data;
    if (sc->len + len < sizeof sc->buf) {
        memcpy(sc->buf + sc->len, chunk, len);
        sc->len += len;
        sc->buf[sc->len] = '\0';
    }
    sc->chunks_count++;
    return 0;
}

TEST_CASE(test_upstream_stream_normal)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock start");

    char url[512];
    snprintf(url, sizeof url, "%s/mock/stream", mock_upstream_base(mu));

    struct stream_capture sc = {0};
    int                   status = 0;

    int rc = upstream_stream_call(
        url, "secret-key", NULL, 0, "{}", 2, 5000, capture_chunk, &sc, &status, NULL, NULL);

    TEST_ASSERT(rc == 0, "stream call rc == 0");
    TEST_ASSERT(status == 200, "stream call status == 200");
    TEST_ASSERT(sc.chunks_count >= 3, "received at least 3 chunks");
    TEST_ASSERT(strstr(sc.buf, "hello") != NULL, "contains hello");
    TEST_ASSERT(strstr(sc.buf, "world") != NULL, "contains world");
    TEST_ASSERT(strstr(sc.buf, "[DONE]") != NULL, "contains [DONE]");

    mock_upstream_stop(mu);
}

TEST_CASE(test_upstream_stream_silence_timeout)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock start");

    char url[512];
    snprintf(url, sizeof url, "%s/mock/stream-slow", mock_upstream_base(mu));

    struct stream_capture sc = {0};
    int                   status = 0;

    /* 200ms silence timeout; upstream sleeps 1200ms between chunks */
    int rc =
        upstream_stream_call(
            url, "secret-key", NULL, 0, "{}", 2, 200, capture_chunk, &sc, &status, NULL, NULL);

    TEST_ASSERT(rc == -110, "stream silence timeout returned -110");

    mock_upstream_stop(mu);
}
