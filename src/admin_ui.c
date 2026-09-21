/** @file admin_ui.c
 *  @brief Web admin dashboard handler and static asset delivery.
 */
#include "admin_ui.h"

/* Embedded HTML content generated at build time */
#include "admin_ui_html.h"

const char*
admin_ui_get_html(size_t* out_len)
{
    if (out_len != NULL) {
        *out_len = g_admin_ui_html_len;
    }
    return (const char*)g_admin_ui_html;
}

int
admin_ui_serve(struct mg_connection* conn)
{
    /* Serve embedded version */
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/html; charset=utf-8\r\n"
              "Content-Length: %zu\r\n"
              "Cache-Control: public, max-age=3600\r\n"
              "Connection: close\r\n\r\n",
              g_admin_ui_html_len);
    mg_write(conn, g_admin_ui_html, g_admin_ui_html_len);
    return 1;
}
