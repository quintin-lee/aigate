/** @file test_gemini_stream.c
 *  @brief Unit tests for Google Gemini SSE streaming bridge state machine (Plan 3, Task 4).
 */
#include "run_tests.h"
#include "provider_adapter.h"
#include "provider_gemini.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char   written[8192];
    size_t written_len;
    int    status;
} gemini_sink_t;

static int
gemini_sink_set_hdr(void* impl, const char* name, const char* value)
{
    (void)impl;
    (void)name;
    (void)value;
    return 0;
}

static int
gemini_sink_write(void* impl, const void* buf, size_t len, bool fin)
{
    (void)fin;
    gemini_sink_t* s = impl;
    if (s->written_len + len < sizeof(s->written)) {
        memcpy(s->written + s->written_len, buf, len);
        s->written_len += len;
        s->written[s->written_len] = '\0';
    }
    return 0;
}

TEST_CASE(test_gemini_streaming_bridge_chunks)
{
    gemini_sink_t sink;
    memset(&sink, 0, sizeof sink);

    aigate_response_ctx rc;
    memset(&rc, 0, sizeof rc);
    rc.impl = &sink;
    rc.set_header = gemini_sink_set_hdr;
    rc.write = gemini_sink_write;

    stream_bridge_t* b = g_provider_gemini.stream_bridge_new(&rc, "gemini-1.5-flash");
    TEST_ASSERT(b != NULL, "bridge new");
    TEST_ASSERT(!g_provider_gemini.stream_bridge_headers_sent(b), "headers not sent initially");

    const char* chunk1 = "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": "
                         "\"Hello\"}], \"role\": \"model\"}}]}\n\n";
    const char* chunk2 = "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \" "
                         "Gemini\"}], \"role\": \"model\"}}]}\n\n";
    const char* chunk3 =
        "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \" streaming!\"}], "
        "\"role\": \"model\"}, \"finishReason\": \"STOP\"}], \"usageMetadata\": "
        "{\"promptTokenCount\": 25, \"candidatesTokenCount\": 10}}\n\n";

    TEST_ASSERT(g_provider_gemini.stream_bridge_feed(b, chunk1, strlen(chunk1)) == 0,
                "feed chunk 1");
    TEST_ASSERT(g_provider_gemini.stream_bridge_headers_sent(b), "headers sent after chunk 1");
    TEST_ASSERT(g_provider_gemini.stream_bridge_feed(b, chunk2, strlen(chunk2)) == 0,
                "feed chunk 2");
    TEST_ASSERT(g_provider_gemini.stream_bridge_feed(b, chunk3, strlen(chunk3)) == 0,
                "feed chunk 3");

    TEST_ASSERT(g_provider_gemini.stream_bridge_finish(b) == 0, "finish ok");

    long ptok = 0, ctok = 0, cached_tok = 0;
    g_provider_gemini.stream_bridge_get_tokens(b, &ptok, &ctok, &cached_tok);

    TEST_ASSERT(ptok == 25, "prompt tokens 25, got %ld", ptok);
    TEST_ASSERT(ctok == 10, "completion tokens 10, got %ld", ctok);

    /* Verify sink received converted OpenAI format chunks */
    TEST_ASSERT(strstr(sink.written, "\"delta\":{\"content\":\"Hello\"}") != NULL, "delta Hello");
    TEST_ASSERT(strstr(sink.written, "\"delta\":{\"content\":\" Gemini\"}") != NULL,
                "delta Gemini");
    TEST_ASSERT(strstr(sink.written, "\"delta\":{\"content\":\" streaming!\"}") != NULL,
                "delta streaming");
    TEST_ASSERT(strstr(sink.written, "\"finish_reason\":\"stop\"") != NULL, "finish_reason stop");
    TEST_ASSERT(strstr(sink.written, "data: [DONE]\n\n") != NULL, "terminal [DONE]");

    g_provider_gemini.stream_bridge_free(b);
}

TEST_CASE(test_gemini_streaming_fragmented_tcp)
{
    gemini_sink_t sink;
    memset(&sink, 0, sizeof sink);

    aigate_response_ctx rc;
    memset(&rc, 0, sizeof rc);
    rc.impl = &sink;
    rc.set_header = gemini_sink_set_hdr;
    rc.write = gemini_sink_write;

    stream_bridge_t* b = g_provider_gemini.stream_bridge_new(&rc, "gemini-1.5-pro");
    TEST_ASSERT(b != NULL, "bridge new");

    const char* full_payload =
        "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"Frag\"}], \"role\": "
        "\"model\"}}]}\n\n"
        "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"mented\"}], \"role\": "
        "\"model\"}, \"finishReason\": \"STOP\"}], \"usageMetadata\": {\"promptTokenCount\": 12, "
        "\"candidatesTokenCount\": 4}}\n\n";

    size_t total_len = strlen(full_payload);
    size_t step = 7; /* Feed 7 bytes at a time */
    for (size_t offset = 0; offset < total_len; offset += step) {
        size_t n = (offset + step <= total_len) ? step : (total_len - offset);
        int    rc_feed = g_provider_gemini.stream_bridge_feed(b, full_payload + offset, n);
        TEST_ASSERT(rc_feed == 0, "feed fragmented chunk offset %zu", offset);
    }

    TEST_ASSERT(g_provider_gemini.stream_bridge_finish(b) == 0, "finish ok");

    long ptok = 0, ctok = 0, cached_tok = 0;
    g_provider_gemini.stream_bridge_get_tokens(b, &ptok, &ctok, &cached_tok);
    TEST_ASSERT(ptok == 12, "ptok 12");
    TEST_ASSERT(ctok == 4, "ctok 4");

    TEST_ASSERT(strstr(sink.written, "\"delta\":{\"content\":\"Frag\"}") != NULL, "frag delta");
    TEST_ASSERT(strstr(sink.written, "\"delta\":{\"content\":\"mented\"}") != NULL, "mented delta");
    TEST_ASSERT(strstr(sink.written, "data: [DONE]\n\n") != NULL, "done frame");

    g_provider_gemini.stream_bridge_free(b);
}

static int
gemini_sink_write_fail(void* impl, const void* buf, size_t len, bool fin)
{
    (void)impl;
    (void)buf;
    (void)len;
    (void)fin;
    return -1; /* simulate client disconnect */
}

TEST_CASE(test_gemini_streaming_client_abort)
{
    gemini_sink_t sink;
    memset(&sink, 0, sizeof sink);

    aigate_response_ctx rc;
    memset(&rc, 0, sizeof rc);
    rc.impl = &sink;
    rc.set_header = gemini_sink_set_hdr;
    rc.write = gemini_sink_write_fail;

    stream_bridge_t* b = g_provider_gemini.stream_bridge_new(&rc, "gemini-1.5-flash");
    TEST_ASSERT(b != NULL, "bridge new");

    /* A content delta triggers an output write; the write fails, so feed must abort. */
    const char* chunk = "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": "
                        "\"Hi\"}], \"role\": \"model\"}}]}\n\n";
    int feed_rc = g_provider_gemini.stream_bridge_feed(b, chunk, strlen(chunk));
    TEST_ASSERT(feed_rc == -1, "feed returns -1 on client abort, got %d", feed_rc);

    g_provider_gemini.stream_bridge_free(b);
}
