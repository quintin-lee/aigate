/** @file mock_upstream.h
 *  @brief In-process blocking-socket mock upstream for unit tests.
 *
 *  One instance per test case; the server thread joins cleanly on stop
 *  (no lingering threads across repeated start/stop cycles).
 */
#ifndef MOCK_UPSTREAM_H
#define MOCK_UPSTREAM_H

#include <stddef.h>

typedef struct mock_upstream mock_upstream_t;

/** @brief Start the mock on a random 127.0.0.1 port.
 * @return new instance, or NULL on failure. */
mock_upstream_t *mock_upstream_start(void);

/** @brief Stop + join the server thread and free the instance.
 *  Safe to call once for a started instance. */
void mock_upstream_stop(mock_upstream_t *mu);

/** @return base URL ("http://127.0.0.1:<port>") for this instance. */
const char *mock_upstream_base(const mock_upstream_t *mu);

/** @brief When @p fail is nonzero, /chat and /chat/completions answer
 *  500 (for upstream-failure pipeline tests). 0 restores 200 behavior. */
void mock_upstream_fail_all(mock_upstream_t *mu, int fail);

/** @brief Total requests served by this instance. */
int mock_upstream_request_count(const mock_upstream_t *mu);

/** @brief The most recently received request body (NUL-terminated,
 *  instance-owned; valid until the next request or stop). */
const char *mock_upstream_last_body(const mock_upstream_t *mu);

/** @brief The most recently received request path. */
const char *mock_upstream_last_path(const mock_upstream_t *mu);

#endif /* MOCK_UPSTREAM_H */
