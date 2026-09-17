/** @file mock_upstream.h
 *  @brief In-process blocking-socket mock upstream (see mock_upstream.c). */
#ifndef MOCK_UPSTREAM_H
#define MOCK_UPSTREAM_H

#include <stddef.h>

extern char g_mock_base[64];
extern int  g_mock_port;

/** @brief Start the mock on a random 127.0.0.1 port.
 * @return base URL ("http://127.0.0.1:<port>"), or NULL on failure.
 * @note A detached pthread owns the listener; call mock_upstream_stop()
 *       exactly once on shutdown. */
const char *mock_upstream_start(void);

/** @brief Stop the listener (safe even if start failed). */
void mock_upstream_stop(void);

#endif /* MOCK_UPSTREAM_H */
