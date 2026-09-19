/** @file admin_ui.h
 *  @brief Web admin dashboard handler and static asset delivery.
 */
#ifndef AIGATE_ADMIN_UI_H
#define AIGATE_ADMIN_UI_H

#include <stddef.h>
#include <civetweb.h>

/**
 * @brief Serve the admin dashboard HTML (from local file or embedded fallback).
 * @param conn CivetWeb connection.
 * @return 1 if handled, 0 otherwise.
 */
int admin_ui_serve(struct mg_connection* conn);

/**
 * @brief Get the embedded HTML content and length.
 * @param out_len Output pointer for byte length.
 * @return Pointer to HTML string data.
 */
const char* admin_ui_get_html(size_t* out_len);

#endif /* AIGATE_ADMIN_UI_H */
