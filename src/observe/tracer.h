/** @file tracer.h
 *  @ingroup group_observe
 *  @brief OpenTelemetry distributed tracing, W3C TraceContext parsing, and span lifecycle.
 */
#ifndef AIGATE_TRACER_H
#define AIGATE_TRACER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum spans recorded per request context (zero dynamic allocation). */
#define TRACE_MAX_SPANS 16

/** @brief Maximum attributes stored per span. */
#define TRACE_MAX_ATTRS 8

/**
 * @brief Span kind following OpenTelemetry specification.
 */
typedef enum {
    SPAN_KIND_INTERNAL = 0, /**< Internal operation within the gateway. */
    SPAN_KIND_SERVER = 1,   /**< Incoming server request from client. */
    SPAN_KIND_CLIENT = 2    /**< Outgoing client request to upstream provider. */
} span_kind_t;

/**
 * @brief Span status code.
 */
typedef enum {
    SPAN_STATUS_UNSET = 0, /**< Default status, neither OK nor error. */
    SPAN_STATUS_OK = 1,    /**< Operation completed successfully. */
    SPAN_STATUS_ERROR = 2  /**< Operation encountered an error. */
} span_status_t;

/**
 * @brief Single span attribute key-value pair.
 */
typedef struct {
    char key[32];   /**< Attribute key name (e.g. gen_ai.system). */
    char value[64]; /**< Attribute string value. */
} span_attr_t;

/**
 * @brief Single trace span representation.
 */
typedef struct {
    char          span_id[17];        /**< 16-hex char span ID with null terminator. */
    char          parent_span_id[17]; /**< 16-hex char parent span ID with null terminator. */
    char          name[32];           /**< Span phase name (e.g. guardrails_inbound). */
    span_kind_t   kind;               /**< Span kind (INTERNAL, SERVER, CLIENT). */
    uint64_t      start_time_ns;      /**< Start monotonic time in nanoseconds. */
    uint64_t      end_time_ns;        /**< End monotonic time in nanoseconds. */
    span_status_t status;             /**< Span completion status. */
    char          status_desc[64];    /**< Error description or message if status is ERROR. */
    span_attr_t   attributes[TRACE_MAX_ATTRS]; /**< Array of key-value attributes. */
    int           attr_count;                  /**< Number of active attributes. */
} trace_span_t;

/**
 * @brief Request trace context (embedded in chat_req_t with zero heap allocation).
 */
typedef struct {
    char         trace_id[33];           /**< 32-hex char trace ID with null terminator. */
    char         root_span_id[17];       /**< 16-hex char root server span ID. */
    char         inbound_parent_id[17];  /**< 16-hex char inbound caller span ID if passed. */
    bool         is_sampled;             /**< Whether this trace is sampled for export. */
    uint8_t      trace_flags;            /**< W3C trace flags byte (e.g. 0x01 = sampled). */
    trace_span_t spans[TRACE_MAX_SPANS]; /**< Fixed array of spans recorded in this request. */
    int          span_count;             /**< Number of active spans recorded. */
    uint64_t     req_start_realtime_us;  /**< CLOCK_REALTIME start timestamp in microseconds. */
} trace_context_t;

/**
 * @brief Initialize a trace context, parsing inbound W3C traceparent header or generating IDs.
 *
 * @param ctx             Trace context to initialize (non-null).
 * @param inbound_header  Inbound W3C "traceparent" header string, or NULL if absent.
 * @param default_sample  Default sampling decision if no inbound sampling flag is present.
 */
void tracer_context_init(trace_context_t* ctx, const char* inbound_header, bool default_sample);

/**
 * @brief Clean up and zero out a trace context.
 *
 * @param ctx Trace context to reset (safe with NULL).
 */
void tracer_context_cleanup(trace_context_t* ctx);

/**
 * @brief Parse a W3C TraceContext traceparent header string.
 *
 * Header format: "00-<32 hex trace_id>-<16 hex parent_id>-<2 hex flags>".
 * Length must be exactly 55 characters, trace_id and parent_id must not be all-zero.
 *
 * @param ctx    Trace context into which trace_id, inbound_parent_id, and flags are saved.
 * @param header Header string to parse.
 * @return true if header is valid and successfully parsed, false otherwise.
 */
bool tracer_parse_traceparent(trace_context_t* ctx, const char* header);

/**
 * @brief Format an outbound W3C traceparent header string for upstream requests.
 *
 * Produces "00-<trace_id>-<span_id>-<flags:02x>".
 *
 * @param ctx     Trace context supplying trace_id and trace_flags.
 * @param span_id Span ID to inject as parent to downstream (defaults to root_span_id if NULL).
 * @param buf     Output buffer.
 * @param buf_sz  Size of output buffer in bytes (must be at least 56).
 */
void tracer_format_traceparent(const trace_context_t* ctx,
                               const char*            span_id,
                               char*                  buf,
                               size_t                 buf_sz);

/**
 * @brief Start a new span in the trace context.
 *
 * Allocates next slot in ctx->spans, assigns a span_id, and records monotonic start timestamp.
 *
 * @param ctx       Trace context.
 * @param name      Span name (e.g. "guardrails_inbound").
 * @param kind      Span kind (INTERNAL, SERVER, CLIENT).
 * @param parent_id Parent span ID (or NULL to default to root_span_id).
 * @return Index of the new span in ctx->spans (0 to TRACE_MAX_SPANS-1), or -1 on error/overflow.
 */
int
tracer_span_start(trace_context_t* ctx, const char* name, span_kind_t kind, const char* parent_id);

/**
 * @brief End an active span in the trace context.
 *
 * Records monotonic end timestamp and sets completion status.
 *
 * @param ctx      Trace context.
 * @param name     Name of the span to end (searches for last active span with this name).
 * @param status   Span status (OK, ERROR, UNSET).
 * @param err_desc Error description string if status is ERROR, or NULL.
 */
void
tracer_span_end(trace_context_t* ctx, const char* name, span_status_t status, const char* err_desc);

/**
 * @brief Set a string attribute on an active or finished span.
 *
 * @param ctx   Trace context.
 * @param name  Span name.
 * @param key   Attribute key string.
 * @param value Attribute value string.
 */
void
tracer_span_set_attr(trace_context_t* ctx, const char* name, const char* key, const char* value);

/**
 * @brief Set an integer attribute on an active or finished span.
 *
 * Formats the integer as a decimal string and saves it.
 *
 * @param ctx   Trace context.
 * @param name  Span name.
 * @param key   Attribute key string.
 * @param value 64-bit integer value.
 */
void
tracer_span_set_attr_int(trace_context_t* ctx, const char* name, const char* key, int64_t value);

/**
 * @brief Configuration parameters for OpenTelemetry tracing and tail sampling.
 */
typedef struct {
    bool     enabled;            /**< Whether tracing is enabled. */
    double   sample_rate;        /**< Probabilistic sample rate 0.0 to 1.0. */
    uint32_t slow_threshold_ms;  /**< Latency threshold (ms) for tail-sampling slow requests. */
    char     otlp_endpoint[256]; /**< OTLP/HTTP collector endpoint URL. */
} tracer_config_t;

/**
 * @brief Evaluate whether a completed request trace should be sampled for export.
 *
 * Implements head and adaptive tail sampling:
 * - Unconditionally false if config is NULL or tracing is disabled.
 * - Always true if trace was already marked sampled (e.g. inbound W3C traceparent flag).
 * - Always true if http_status >= 400 (tail-sampling errors and guardrail blocks).
 * - Always true if elapsed_ms >= cfg->slow_threshold_ms (tail-sampling slow queries).
 * - Probabilistic sampling according to cfg->sample_rate (0.0 to 1.0).
 *
 * @param ctx         Trace context for the request (may be NULL).
 * @param cfg         Tracer configuration (may be NULL).
 * @param http_status HTTP status code returned for the request.
 * @param elapsed_ms  Total request elapsed time in milliseconds.
 * @return true if the trace should be sampled and retained, false otherwise.
 */
bool tracer_should_sample(const trace_context_t* ctx,
                          const tracer_config_t* cfg,
                          int                    http_status,
                          uint64_t               elapsed_ms);

/** @brief Default capacity for trace ring buffer. */
#define TRACE_RING_BUFFER_DEFAULT_CAPACITY 1024

/**
 * @brief Thread-safe ring buffer for queued trace contexts awaiting export.
 */
typedef struct trace_ring_buffer trace_ring_buffer_t;

/**
 * @brief Create a new thread-safe trace ring buffer.
 *
 * @param capacity Maximum number of trace contexts to buffer (0 for default 1024).
 * @return Pointer to newly allocated ring buffer, or NULL on allocation failure.
 */
trace_ring_buffer_t* trace_ring_buffer_create(size_t capacity);

/**
 * @brief Destroy a trace ring buffer and free all associated resources.
 *
 * @param rb Ring buffer to destroy (safe with NULL).
 */
void trace_ring_buffer_destroy(trace_ring_buffer_t* rb);

/**
 * @brief Non-blocking push of a trace context into the ring buffer.
 *
 * If the ring buffer is full, the oldest trace context is overwritten and the
 * dropped count is incremented. This function never blocks the caller.
 *
 * @param rb  Ring buffer instance.
 * @param ctx Trace context to copy into the buffer.
 * @return true on success, false if parameters are invalid.
 */
bool trace_ring_buffer_push(trace_ring_buffer_t* rb, const trace_context_t* ctx);

/**
 * @brief Pop a trace context from the ring buffer, waiting up to timeout_ms if empty.
 *
 * @param rb         Ring buffer instance.
 * @param out_ctx    Destination buffer for popped trace context.
 * @param timeout_ms Maximum time to wait in milliseconds if buffer is empty.
 * @return true if a trace was popped, false if timed out or parameters are invalid.
 */
bool trace_ring_buffer_pop(trace_ring_buffer_t* rb, trace_context_t* out_ctx, uint32_t timeout_ms);

/**
 * @brief Get the current number of trace contexts stored in the ring buffer.
 *
 * @param rb Ring buffer instance.
 * @return Number of queued items.
 */
size_t trace_ring_buffer_count(trace_ring_buffer_t* rb);

/**
 * @brief Get the cumulative count of traces dropped due to ring buffer overflow.
 *
 * @param rb Ring buffer instance.
 * @return Total number of dropped traces.
 */
uint64_t trace_ring_buffer_dropped(trace_ring_buffer_t* rb);

/**
 * @brief Serialize an array of trace contexts into standard OTLP/HTTP JSON string.
 *
 * Output complies with OpenTelemetry Traces OTLP/HTTP JSON schema (resourceSpans, scopeSpans).
 * Caller is responsible for freeing the returned string using free().
 *
 * @param traces Array of trace contexts to serialize.
 * @param count  Number of trace contexts in array.
 * @return Dynamically allocated JSON string, or NULL on error or empty input.
 */
char* tracer_serialize_otlp_json(const trace_context_t* traces, int count);

/** @brief Default capacity for recent traces cache for web console inspection. */
#define TRACE_RECENT_CACHE_CAPACITY 500

/**
 * @brief Store a trace context in the recent traces memory index.
 *
 * Thread-safe. Overwrites the oldest trace if cache capacity is exceeded.
 *
 * @param ctx Trace context to record.
 */
void tracer_cache_add(const trace_context_t* ctx);

/**
 * @brief Look up a trace context by its 32-hex trace ID in the recent cache.
 *
 * Thread-safe.
 *
 * @param trace_id 32-hex character trace ID string.
 * @param out_ctx  Destination buffer to copy the found trace context into.
 * @return true if found, false if not found or parameters are invalid.
 */
bool tracer_cache_get(const char* trace_id, trace_context_t* out_ctx);

/**
 * @brief Retrieve a list of recently recorded trace contexts.
 *
 * Thread-safe. Copies up to max_count trace contexts in reverse chronological order.
 *
 * @param out_array Destination array of trace contexts.
 * @param max_count Maximum number of trace contexts to copy.
 * @return Number of trace contexts copied into out_array.
 */
size_t tracer_cache_list_recent(trace_context_t* out_array, size_t max_count);

/**
 * @brief Clear all cached traces in the recent traces memory index.
 */
void tracer_cache_clear(void);

/**
 * @brief Background tracer worker manager and OTLP exporter.
 */
typedef struct tracer_manager tracer_manager_t;

/**
 * @brief Create and start a background tracer manager with worker thread.
 *
 * @param cfg Initial tracer configuration (may be NULL for defaults).
 * @param rb  Trace ring buffer from which traces are consumed (non-null).
 * @return Newly allocated tracer manager, or NULL on error.
 */
tracer_manager_t* tracer_manager_create(const tracer_config_t* cfg, trace_ring_buffer_t* rb);

/**
 * @brief Stop the background exporter worker thread.
 *
 * @param tm Tracer manager instance (safe with NULL).
 */
void tracer_manager_stop(tracer_manager_t* tm);

/**
 * @brief Destroy a tracer manager and release its resources.
 *
 * Calls tracer_manager_stop if still running.
 *
 * @param tm Tracer manager instance (safe with NULL).
 */
void tracer_manager_destroy(tracer_manager_t* tm);

/**
 * @brief Dynamically update tracer configuration at runtime.
 *
 * Thread-safe.
 *
 * @param tm      Tracer manager instance.
 * @param new_cfg New configuration parameters.
 */
void tracer_manager_update_config(tracer_manager_t* tm, const tracer_config_t* new_cfg);

/**
 * @brief Retrieve the current tracer configuration.
 *
 * Thread-safe.
 *
 * @param tm Tracer manager instance.
 * @return Current configuration copy.
 */
tracer_config_t tracer_manager_get_config(const tracer_manager_t* tm);

#endif /* AIGATE_TRACER_H */
