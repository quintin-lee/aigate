#!/ usr / bin / env python3
"""Converts schema/schema.sql into src/store/schema_sql.h with an embedded C string literal."""

import os
import sys

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(root, "schema", "schema.sql")
    dst = os.path.join(root, "src", "store", "schema_sql.h")

    with open(src, "r", encoding="utf-8") as f:
        sql = f.read()

    lines = sql.splitlines()
    formatted = []
    for line in lines:
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        formatted.append('    "' + escaped + '\\n"')

    header = """/** @file schema_sql.h
 *  @ingroup group_store
 *  @brief Embedded copy of schema/schema.sql (generated; do not edit by hand). */
#ifndef AIGATE_SCHEMA_SQL_H
#define AIGATE_SCHEMA_SQL_H

#define AIGATE_SCHEMA_VERSION 17

static const char SCHEMA_SQL[] =
"""

    content = header + "\n".join(formatted) + ";\n\n#endif /* AIGATE_SCHEMA_SQL_H */\n"
    with open(dst, "w", encoding="utf-8") as f:
        f.write(content)

if __name__ == "__main__":
    main()
