/** @file tracer.c
 *  @brief OpenTelemetry distributed tracing and W3C TraceContext implementation.
 */
#include "tracer.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Fill buffer with cryptographically secure random bytes with fallback.
 */
static void
get_random_bytes(uint8_t* out, size_t len)
{
    if (RAND_bytes(out, (int)len) == 1) {
        return;
    }

    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t r = read(fd, out, len);
        close(fd);
        if (r == (ssize_t)len) {
            return;
        }
    }

    /* Fallback to pseudo-random generator */
    static _Thread_local unsigned int seed = 0;
    if (seed == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        seed = (unsigned int)(ts.tv_nsec ^ (uintptr_t)&seed ^ (unsigned int)getpid());
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(rand_r(&seed) & 0xFF);
    }
}

/**
 * @brief Generate a non-all-zero hexadecimal string of the specified character length.
 */
static void
generate_hex_id(char* out, size_t hex_len)
{
    size_t  byte_len = hex_len / 2;
    uint8_t bytes[16];
    if (byte_len > sizeof(bytes)) {
        byte_len = sizeof(bytes);
    }

    /* Loop until at least one non-zero byte to guarantee non-all-zero */
    while (1) {
        get_random_bytes(bytes, byte_len);
        bool all_zero = true;
        for (size_t i = 0; i < byte_len; i++) {
            if (bytes[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (!all_zero) {
            break;
        }
    }

    static const char hex_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < byte_len; i++) {
        out[i * 2] = hex_digits[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex_digits[bytes[i] & 0x0F];
    }
    out[hex_len] = '\0';
}

/**
 * @brief Convert single hex character to nibble value, or -1 if invalid.
 */
static inline int
hex_char_to_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool
tracer_parse_traceparent(trace_context_t* ctx, const char* header)
{
    if (!ctx || !header) {
        return false;
    }

    /* W3C traceparent length is exactly 55 characters:
     * 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01 */
    if (strlen(header) != 55) {
        return false;
    }

    /* Delimiters check */
    if (header[2] != '-' || header[35] != '-' || header[52] != '-') {
        return false;
    }

    /* Version check: must be "00". In W3C, "ff" is forbidden. */
    if (header[0] != '0' || header[1] != '0') {
        return false;
    }

    /* Trace ID check: 32 hex chars, not all zeros */
    bool trace_id_nonzero = false;
    for (int i = 3; i < 35; i++) {
        if (!isxdigit((unsigned char)header[i])) {
            return false;
        }
        if (header[i] != '0') {
            trace_id_nonzero = true;
        }
    }
    if (!trace_id_nonzero) {
        return false;
    }

    /* Parent ID check: 16 hex chars, not all zeros */
    bool parent_id_nonzero = false;
    for (int i = 36; i < 52; i++) {
        if (!isxdigit((unsigned char)header[i])) {
            return false;
        }
        if (header[i] != '0') {
            parent_id_nonzero = true;
        }
    }
    if (!parent_id_nonzero) {
        return false;
    }

    /* Trace flags check: 2 hex chars */
    int f1 = hex_char_to_val(header[53]);
    int f2 = hex_char_to_val(header[54]);
    if (f1 < 0 || f2 < 0) {
        return false;
    }
    uint8_t flags = (uint8_t)((f1 << 4) | f2);

    /* Valid: populate context */
    for (int i = 0; i < 32; i++) {
        ctx->trace_id[i] = (char)tolower((unsigned char)header[3 + i]);
    }
    ctx->trace_id[32] = '\0';

    for (int i = 0; i < 16; i++) {
        ctx->inbound_parent_id[i] = (char)tolower((unsigned char)header[36 + i]);
    }
    ctx->inbound_parent_id[16] = '\0';

    ctx->trace_flags = flags;
    ctx->is_sampled = (flags & 0x01) != 0;

    return true;
}

void
tracer_format_traceparent(const trace_context_t* ctx, const char* span_id, char* buf, size_t buf_sz)
{
    if (!ctx || !buf || buf_sz == 0) {
        return;
    }

    const char* sid = (span_id && span_id[0] != '\0') ? span_id : ctx->root_span_id;
    uint8_t     flags = ctx->trace_flags;
    if (ctx->is_sampled) {
        flags |= 0x01;
    } else {
        flags &= ~0x01;
    }

    snprintf(buf, buf_sz, "00-%s-%s-%02x", ctx->trace_id, sid, flags);
}

void
tracer_context_init(trace_context_t* ctx, const char* inbound_header, bool default_sample)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ctx->req_start_realtime_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;

    bool parsed = false;
    if (inbound_header != NULL && inbound_header[0] != '\0') {
        parsed = tracer_parse_traceparent(ctx, inbound_header);
    }

    if (!parsed) {
        generate_hex_id(ctx->trace_id, 32);
        ctx->inbound_parent_id[0] = '\0';
        ctx->is_sampled = default_sample;
        ctx->trace_flags = default_sample ? 0x01 : 0x00;
    }

    /* Always generate a root span ID for this gateway process */
    generate_hex_id(ctx->root_span_id, 16);
    ctx->span_count = 0;
}

void
tracer_context_cleanup(trace_context_t* ctx)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
}

int
tracer_span_start(trace_context_t* ctx, const char* name, span_kind_t kind, const char* parent_id)
{
    if (!ctx || !name) {
        return -1;
    }
    if (ctx->span_count >= TRACE_MAX_SPANS) {
        return -1;
    }

    int           idx = ctx->span_count++;
    trace_span_t* s = &ctx->spans[idx];
    memset(s, 0, sizeof(*s));

    if (strcmp(name, "root") == 0 && ctx->root_span_id[0] != '\0') {
        strncpy(s->span_id, ctx->root_span_id, sizeof(s->span_id) - 1);
    } else {
        generate_hex_id(s->span_id, 16);
    }

    if (parent_id && parent_id[0] != '\0') {
        strncpy(s->parent_span_id, parent_id, sizeof(s->parent_span_id) - 1);
    } else if (strcmp(name, "root") == 0) {
        if (ctx->inbound_parent_id[0] != '\0') {
            strncpy(s->parent_span_id, ctx->inbound_parent_id, sizeof(s->parent_span_id) - 1);
        }
    } else if (ctx->root_span_id[0] != '\0') {
        strncpy(s->parent_span_id, ctx->root_span_id, sizeof(s->parent_span_id) - 1);
    }

    strncpy(s->name, name, sizeof(s->name) - 1);
    s->kind = kind;
    s->status = SPAN_STATUS_UNSET;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s->start_time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    return idx;
}

void
tracer_span_end(trace_context_t* ctx, const char* name, span_status_t status, const char* err_desc)
{
    if (!ctx || !name) {
        return;
    }

    trace_span_t* target = NULL;
    for (int i = ctx->span_count - 1; i >= 0; i--) {
        if (strcmp(ctx->spans[i].name, name) == 0 && ctx->spans[i].end_time_ns == 0) {
            target = &ctx->spans[i];
            break;
        }
    }
    if (!target) {
        for (int i = ctx->span_count - 1; i >= 0; i--) {
            if (strcmp(ctx->spans[i].name, name) == 0) {
                target = &ctx->spans[i];
                break;
            }
        }
    }
    if (!target) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    target->end_time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    if (target->end_time_ns < target->start_time_ns) {
        target->end_time_ns = target->start_time_ns;
    }

    target->status = status;
    if (err_desc && err_desc[0] != '\0') {
        strncpy(target->status_desc, err_desc, sizeof(target->status_desc) - 1);
        target->status_desc[sizeof(target->status_desc) - 1] = '\0';
    }
}

void
tracer_span_set_attr(trace_context_t* ctx, const char* name, const char* key, const char* value)
{
    if (!ctx || !name || !key || !value) {
        return;
    }

    trace_span_t* target = NULL;
    for (int i = ctx->span_count - 1; i >= 0; i--) {
        if (strcmp(ctx->spans[i].name, name) == 0) {
            target = &ctx->spans[i];
            if (target->end_time_ns == 0) {
                break;
            }
        }
    }
    if (!target) {
        return;
    }

    /* Check if attribute key already exists; if so, overwrite */
    for (int i = 0; i < target->attr_count; i++) {
        if (strcmp(target->attributes[i].key, key) == 0) {
            strncpy(target->attributes[i].value, value, sizeof(target->attributes[i].value) - 1);
            target->attributes[i].value[sizeof(target->attributes[i].value) - 1] = '\0';
            return;
        }
    }

    /* If not found, append if within capacity */
    if (target->attr_count < TRACE_MAX_ATTRS) {
        span_attr_t* attr = &target->attributes[target->attr_count++];
        strncpy(attr->key, key, sizeof(attr->key) - 1);
        attr->key[sizeof(attr->key) - 1] = '\0';
        strncpy(attr->value, value, sizeof(attr->value) - 1);
        attr->value[sizeof(attr->value) - 1] = '\0';
    }
}

void
tracer_span_set_attr_int(trace_context_t* ctx, const char* name, const char* key, int64_t value)
{
    char val_buf[32];
    snprintf(val_buf, sizeof(val_buf), "%" PRId64, value);
    tracer_span_set_attr(ctx, name, key, val_buf);
}

bool
tracer_should_sample(const trace_context_t* ctx,
                     const tracer_config_t* cfg,
                     int                    http_status,
                     uint64_t               elapsed_ms)
{
    if (!cfg || !cfg->enabled) {
        return false;
    }

    if (ctx && ctx->is_sampled) {
        return true;
    }

    if (http_status >= 400) {
        return true;
    }

    if (cfg->slow_threshold_ms > 0 && elapsed_ms >= cfg->slow_threshold_ms) {
        return true;
    }

    if (cfg->sample_rate >= 1.0) {
        return true;
    }

    if (cfg->sample_rate <= 0.0) {
        return false;
    }

    double roll = (double)rand() / (double)RAND_MAX;
    return roll < cfg->sample_rate;
}

/**
 * @brief Internal implementation of the thread-safe trace ring buffer.
 */
struct trace_ring_buffer {
    trace_context_t* items;         /**< Dynamically allocated array of trace contexts. */
    size_t           capacity;      /**< Buffer capacity. */
    size_t           head;          /**< Read index. */
    size_t           tail;          /**< Write index. */
    size_t           count;         /**< Number of items currently in buffer. */
    uint64_t         dropped_count; /**< Cumulative dropped items due to overflow. */
    pthread_mutex_t  lock;          /**< Mutex protecting buffer operations. */
    pthread_cond_t   not_empty;     /**< Condition variable signaled on new items. */
};

trace_ring_buffer_t*
trace_ring_buffer_create(size_t capacity)
{
    if (capacity == 0) {
        capacity = TRACE_RING_BUFFER_DEFAULT_CAPACITY;
    }

    trace_ring_buffer_t* rb = calloc(1, sizeof(trace_ring_buffer_t));
    if (!rb) {
        return NULL;
    }

    rb->items = calloc(capacity, sizeof(trace_context_t));
    if (!rb->items) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->dropped_count = 0;

    if (pthread_mutex_init(&rb->lock, NULL) != 0) {
        free(rb->items);
        free(rb);
        return NULL;
    }

    if (pthread_cond_init(&rb->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&rb->lock);
        free(rb->items);
        free(rb);
        return NULL;
    }

    return rb;
}

void
trace_ring_buffer_destroy(trace_ring_buffer_t* rb)
{
    if (!rb) {
        return;
    }

    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->not_empty);

    free(rb->items);
    free(rb);
}

bool
trace_ring_buffer_push(trace_ring_buffer_t* rb, const trace_context_t* ctx)
{
    if (!rb || !ctx) {
        return false;
    }

    pthread_mutex_lock(&rb->lock);

    if (rb->count == rb->capacity) {
        /* Buffer is full: overwrite oldest item at head without blocking */
        rb->items[rb->head] = *ctx;
        rb->head = (rb->head + 1) % rb->capacity;
        rb->tail = rb->head;
        rb->dropped_count++;
    } else {
        rb->items[rb->tail] = *ctx;
        rb->tail = (rb->tail + 1) % rb->capacity;
        rb->count++;
    }

    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);

    return true;
}

bool
trace_ring_buffer_pop(trace_ring_buffer_t* rb, trace_context_t* out_ctx, uint32_t timeout_ms)
{
    if (!rb || !out_ctx) {
        return false;
    }

    pthread_mutex_lock(&rb->lock);

    if (rb->count == 0 && timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_ms * 1000000ULL;
        ts.tv_sec += (time_t)(nsec / 1000000000ULL);
        ts.tv_nsec = (long)(nsec % 1000000000ULL);

        while (rb->count == 0) {
            int rc = pthread_cond_timedwait(&rb->not_empty, &rb->lock, &ts);
            if (rc != 0) {
                break;
            }
        }
    }

    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->lock);
        return false;
    }

    *out_ctx = rb->items[rb->head];
    rb->head = (rb->head + 1) % rb->capacity;
    rb->count--;

    pthread_mutex_unlock(&rb->lock);
    return true;
}

size_t
trace_ring_buffer_count(trace_ring_buffer_t* rb)
{
    if (!rb) {
        return 0;
    }
    pthread_mutex_lock(&rb->lock);
    size_t count = rb->count;
    pthread_mutex_unlock(&rb->lock);
    return count;
}

uint64_t
trace_ring_buffer_dropped(trace_ring_buffer_t* rb)
{
    if (!rb) {
        return 0;
    }
    pthread_mutex_lock(&rb->lock);
    uint64_t dropped = rb->dropped_count;
    pthread_mutex_unlock(&rb->lock);
    return dropped;
}
