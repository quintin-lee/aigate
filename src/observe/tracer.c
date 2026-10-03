/** @file tracer.c
 *  @brief OpenTelemetry distributed tracing and W3C TraceContext implementation.
 */
#include "tracer.h"
#include "aigate_log.h"
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <jansson.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdatomic.h>
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
        snprintf(s->span_id, sizeof(s->span_id), "%s", ctx->root_span_id);
    } else {
        generate_hex_id(s->span_id, 16);
    }

    if (parent_id && parent_id[0] != '\0') {
        snprintf(s->parent_span_id, sizeof(s->parent_span_id), "%s", parent_id);
    } else if (strcmp(name, "root") == 0) {
        if (ctx->inbound_parent_id[0] != '\0') {
            snprintf(s->parent_span_id, sizeof(s->parent_span_id), "%s", ctx->inbound_parent_id);
        }
    } else if (ctx->root_span_id[0] != '\0') {
        snprintf(s->parent_span_id, sizeof(s->parent_span_id), "%s", ctx->root_span_id);
    }

    snprintf(s->name, sizeof(s->name), "%s", name);
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

char*
tracer_serialize_otlp_json(const trace_context_t* traces, int count)
{
    if (traces == NULL || count <= 0) {
        return NULL;
    }

    json_t* root = json_object();
    json_t* res_spans_arr = json_array();

    json_t* res_span_obj = json_object();

    /* 1. Resource: service.name & service.version */
    json_t* resource_obj = json_object();
    json_t* res_attrs_arr = json_array();

    json_t* attr_svc = json_object();
    json_object_set_new(attr_svc, "key", json_string("service.name"));
    json_t* val_svc = json_object();
    json_object_set_new(val_svc, "stringValue", json_string("aigate"));
    json_object_set_new(attr_svc, "value", val_svc);
    json_array_append_new(res_attrs_arr, attr_svc);

    json_t* attr_ver = json_object();
    json_object_set_new(attr_ver, "key", json_string("service.version"));
    json_t* val_ver = json_object();
    json_object_set_new(val_ver, "stringValue", json_string("0.1.0"));
    json_object_set_new(attr_ver, "value", val_ver);
    json_array_append_new(res_attrs_arr, attr_ver);

    json_object_set_new(resource_obj, "attributes", res_attrs_arr);
    json_object_set_new(res_span_obj, "resource", resource_obj);

    /* 2. ScopeSpans */
    json_t* scope_spans_arr = json_array();
    json_t* scope_span_obj = json_object();

    json_t* scope_obj = json_object();
    json_object_set_new(scope_obj, "name", json_string("aigate.tracer"));
    json_object_set_new(scope_obj, "version", json_string("1.0.0"));
    json_object_set_new(scope_span_obj, "scope", scope_obj);

    json_t* spans_arr = json_array();
    for (int t = 0; t < count; t++) {
        const trace_context_t* ctx = &traces[t];
        uint64_t               root_start_mono = 0;
        if (ctx->span_count > 0) {
            root_start_mono = ctx->spans[0].start_time_ns;
        }

        for (int s = 0; s < ctx->span_count; s++) {
            const trace_span_t* span = &ctx->spans[s];
            json_t*             span_obj = json_object();

            json_object_set_new(span_obj, "traceId", json_string(ctx->trace_id));
            json_object_set_new(span_obj, "spanId", json_string(span->span_id));
            if (span->parent_span_id[0] != '\0') {
                json_object_set_new(span_obj, "parentSpanId", json_string(span->parent_span_id));
            } else if (ctx->inbound_parent_id[0] != '\0' && s == 0) {
                json_object_set_new(span_obj, "parentSpanId", json_string(ctx->inbound_parent_id));
            }

            json_object_set_new(span_obj, "name", json_string(span->name));

            /* Map span_kind_t: INTERNAL=1, SERVER=2, CLIENT=3 */
            int otlp_kind = 1;
            if (span->kind == SPAN_KIND_SERVER) {
                otlp_kind = 2;
            } else if (span->kind == SPAN_KIND_CLIENT) {
                otlp_kind = 3;
            }
            json_object_set_new(span_obj, "kind", json_integer(otlp_kind));

            /* Nanosecond Unix epoch timestamps */
            uint64_t offset_ns = (span->start_time_ns >= root_start_mono)
                                     ? (span->start_time_ns - root_start_mono)
                                     : 0;
            uint64_t dur_ns = (span->end_time_ns >= span->start_time_ns)
                                  ? (span->end_time_ns - span->start_time_ns)
                                  : 0;
            uint64_t start_unix_ns = ctx->req_start_realtime_us * 1000ULL + offset_ns;
            uint64_t end_unix_ns = start_unix_ns + dur_ns;

            char start_str[32];
            char end_str[32];
            snprintf(start_str, sizeof(start_str), "%" PRIu64, start_unix_ns);
            snprintf(end_str, sizeof(end_str), "%" PRIu64, end_unix_ns);
            json_object_set_new(span_obj, "startTimeUnixNano", json_string(start_str));
            json_object_set_new(span_obj, "endTimeUnixNano", json_string(end_str));

            /* Attributes */
            json_t* attrs_arr = json_array();
            for (int a = 0; a < span->attr_count; a++) {
                json_t* attr = json_object();
                json_object_set_new(attr, "key", json_string(span->attributes[a].key));
                json_t* v_obj = json_object();
                json_object_set_new(v_obj, "stringValue", json_string(span->attributes[a].value));
                json_object_set_new(attr, "value", v_obj);
                json_array_append_new(attrs_arr, attr);
            }
            json_object_set_new(span_obj, "attributes", attrs_arr);

            /* Status: code 1 = OK, code 2 = ERROR */
            json_t* status_obj = json_object();
            int     st_code = (span->status == SPAN_STATUS_ERROR) ? 2 : 1;
            json_object_set_new(status_obj, "code", json_integer(st_code));
            if (span->status_desc[0] != '\0') {
                json_object_set_new(status_obj, "message", json_string(span->status_desc));
            }
            json_object_set_new(span_obj, "status", status_obj);

            json_array_append_new(spans_arr, span_obj);
        }
    }

    json_object_set_new(scope_span_obj, "spans", spans_arr);
    json_array_append_new(scope_spans_arr, scope_span_obj);
    json_object_set_new(res_span_obj, "scopeSpans", scope_spans_arr);

    json_array_append_new(res_spans_arr, res_span_obj);
    json_object_set_new(root, "resourceSpans", res_spans_arr);

    char* out = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return out;
}

/**
 * @brief Thread-safe circular array holding recent trace records for Web console queries.
 */
typedef struct {
    trace_context_t items[TRACE_RECENT_CACHE_CAPACITY]; /**< Array of cached traces. */
    size_t          count;                              /**< Current number of cached traces. */
    size_t          next_idx;                           /**< Next write slot index. */
    pthread_mutex_t mtx;                                /**< Mutex protecting cache access. */
} trace_recent_cache_t;

static trace_recent_cache_t g_recent_cache = {
    .count = 0, .next_idx = 0, .mtx = PTHREAD_MUTEX_INITIALIZER};

void
tracer_cache_add(const trace_context_t* ctx)
{
    if (ctx == NULL || ctx->trace_id[0] == '\0') {
        return;
    }
    pthread_mutex_lock(&g_recent_cache.mtx);
    size_t idx = g_recent_cache.next_idx;
    g_recent_cache.items[idx] = *ctx;
    g_recent_cache.next_idx = (idx + 1) % TRACE_RECENT_CACHE_CAPACITY;
    if (g_recent_cache.count < TRACE_RECENT_CACHE_CAPACITY) {
        g_recent_cache.count++;
    }
    pthread_mutex_unlock(&g_recent_cache.mtx);
}

bool
tracer_cache_get(const char* trace_id, trace_context_t* out_ctx)
{
    if (trace_id == NULL || out_ctx == NULL) {
        return false;
    }
    pthread_mutex_lock(&g_recent_cache.mtx);
    for (size_t i = 0; i < g_recent_cache.count; i++) {
        if (strcasecmp(g_recent_cache.items[i].trace_id, trace_id) == 0) {
            *out_ctx = g_recent_cache.items[i];
            pthread_mutex_unlock(&g_recent_cache.mtx);
            return true;
        }
    }
    pthread_mutex_unlock(&g_recent_cache.mtx);
    return false;
}

size_t
tracer_cache_list_recent(trace_context_t* out_array, size_t max_count)
{
    if (out_array == NULL || max_count == 0) {
        return 0;
    }
    pthread_mutex_lock(&g_recent_cache.mtx);
    size_t n = (g_recent_cache.count < max_count) ? g_recent_cache.count : max_count;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (g_recent_cache.next_idx + TRACE_RECENT_CACHE_CAPACITY - 1 - i) %
                     TRACE_RECENT_CACHE_CAPACITY;
        out_array[i] = g_recent_cache.items[idx];
    }
    pthread_mutex_unlock(&g_recent_cache.mtx);
    return n;
}

void
tracer_cache_clear(void)
{
    pthread_mutex_lock(&g_recent_cache.mtx);
    g_recent_cache.count = 0;
    g_recent_cache.next_idx = 0;
    pthread_mutex_unlock(&g_recent_cache.mtx);
}

/**
 * @brief Internal implementation of background tracer manager.
 */
struct tracer_manager {
    tracer_config_t      cfg;        /**< Active tracer configuration copy. */
    pthread_mutex_t      cfg_mtx;    /**< Mutex protecting configuration reads/writes. */
    trace_ring_buffer_t* rb;         /**< Reference to shared ring buffer queue. */
    pthread_t            worker_tid; /**< Worker pthread handle. */
    atomic_bool          running;    /**< Flag controlling worker loop execution. */
};

static void*
trace_exporter_worker(void* arg)
{
    tracer_manager_t* tm = (tracer_manager_t*)arg;
    trace_context_t   batch[50];
    time_t            last_warn_time = 0;

    while (atomic_load(&tm->running)) {
        int             count = 0;
        trace_context_t ctx;

        /* Pop first item waiting up to 1000ms */
        if (trace_ring_buffer_pop(tm->rb, &ctx, 1000)) {
            batch[count++] = ctx;
            tracer_cache_add(&ctx);

            /* Drain up to 49 more non-blocking */
            while (count < 50 && trace_ring_buffer_pop(tm->rb, &ctx, 0)) {
                batch[count++] = ctx;
                tracer_cache_add(&ctx);
            }
        }

        if (count == 0) {
            continue;
        }

        pthread_mutex_lock(&tm->cfg_mtx);
        char endpoint[256];
        strncpy(endpoint, tm->cfg.otlp_endpoint, sizeof(endpoint) - 1);
        endpoint[sizeof(endpoint) - 1] = '\0';
        bool enabled = tm->cfg.enabled;
        pthread_mutex_unlock(&tm->cfg_mtx);

        if (!enabled || endpoint[0] == '\0') {
            continue;
        }

        char* json_str = tracer_serialize_otlp_json(batch, count);
        if (json_str == NULL) {
            continue;
        }

        CURL* c = curl_easy_init();
        if (c != NULL) {
            struct curl_slist* hdrs = NULL;
            hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
            hdrs = curl_slist_append(hdrs, "User-Agent: aigate-tracer/1.0");

            curl_easy_setopt(c, CURLOPT_URL, endpoint);
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_str);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(json_str));
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
            curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 2000L);
            curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
            curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

            CURLcode res = curl_easy_perform(c);
            if (res != CURLE_OK) {
                time_t now = time(NULL);
                if (now - last_warn_time > 60) {
                    last_warn_time = now;
                    AIGATE_LOG_WARN("OTLP exporter failed to send %d traces to %s: %s",
                                    count,
                                    endpoint,
                                    curl_easy_strerror(res));
                }
            }
            curl_slist_free_all(hdrs);
            curl_easy_cleanup(c);
        }
        free(json_str);
    }

    /* Drain remaining queued traces into local cache on shutdown */
    trace_context_t ctx;
    while (trace_ring_buffer_pop(tm->rb, &ctx, 0)) {
        tracer_cache_add(&ctx);
    }
    return NULL;
}

tracer_manager_t*
tracer_manager_create(const tracer_config_t* cfg, trace_ring_buffer_t* rb)
{
    if (rb == NULL) {
        return NULL;
    }

    tracer_manager_t* tm = calloc(1, sizeof(tracer_manager_t));
    if (!tm) {
        return NULL;
    }

    if (cfg) {
        tm->cfg = *cfg;
    } else {
        tm->cfg.enabled = true;
        tm->cfg.sample_rate = 1.0;
        tm->cfg.slow_threshold_ms = 2000;
        tm->cfg.otlp_endpoint[0] = '\0';
    }

    if (pthread_mutex_init(&tm->cfg_mtx, NULL) != 0) {
        free(tm);
        return NULL;
    }

    tm->rb = rb;
    atomic_init(&tm->running, true);

    if (pthread_create(&tm->worker_tid, NULL, trace_exporter_worker, tm) != 0) {
        pthread_mutex_destroy(&tm->cfg_mtx);
        free(tm);
        return NULL;
    }

    return tm;
}

void
tracer_manager_stop(tracer_manager_t* tm)
{
    if (tm == NULL) {
        return;
    }

    if (atomic_exchange(&tm->running, false)) {
        /* Wake up worker if blocked in timedwait */
        trace_context_t dummy;
        memset(&dummy, 0, sizeof(dummy));
        trace_ring_buffer_push(tm->rb, &dummy);
        pthread_join(tm->worker_tid, NULL);
    }
}

void
tracer_manager_destroy(tracer_manager_t* tm)
{
    if (tm == NULL) {
        return;
    }
    tracer_manager_stop(tm);
    pthread_mutex_destroy(&tm->cfg_mtx);
    free(tm);
}

void
tracer_manager_update_config(tracer_manager_t* tm, const tracer_config_t* new_cfg)
{
    if (tm == NULL || new_cfg == NULL) {
        return;
    }
    pthread_mutex_lock(&tm->cfg_mtx);
    tm->cfg = *new_cfg;
    pthread_mutex_unlock(&tm->cfg_mtx);
}

tracer_config_t
tracer_manager_get_config(const tracer_manager_t* tm)
{
    tracer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (tm == NULL) {
        return cfg;
    }
    pthread_mutex_lock((pthread_mutex_t*)&tm->cfg_mtx);
    cfg = tm->cfg;
    pthread_mutex_unlock((pthread_mutex_t*)&tm->cfg_mtx);
    return cfg;
}
