/** @file admin_ui.c
 *  @brief Web admin dashboard handler and static asset delivery.
 */
#include "admin_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    /* Check if local override web/admin.html exists for live frontend dev */
    FILE* f = fopen("web/admin.html", "rb");
    if (f != NULL) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            if (sz > 0 && fseek(f, 0, SEEK_SET) == 0) {
                char* buf = malloc((size_t)sz);
                if (buf != NULL && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                    fclose(f);
                    mg_printf(conn,
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html; charset=utf-8\r\n"
                              "Content-Length: %zu\r\n"
                              "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                              "Connection: close\r\n\r\n",
                              (size_t)sz);
                    mg_write(conn, buf, (size_t)sz);
                    free(buf);
                    return 1;
                }
                free(buf);
            }
        }
        fclose(f);
    }

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
