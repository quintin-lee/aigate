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

#endif /* AIGATE_TRACER_H */
