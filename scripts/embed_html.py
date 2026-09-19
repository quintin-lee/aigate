#!/usr/bin/env python3
"""Converts an HTML file into a C header with an embedded static byte array."""

import sys
import os

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input_html> <output_header>")
        sys.exit(1)

    src = sys.argv[1]
    dst = sys.argv[2]

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)

    with open(src, "rb") as f:
        data = f.read()

    with open(dst, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated from web/admin.html - DO NOT EDIT */\n")
        f.write("#ifndef AIGATE_ADMIN_UI_HTML_H\n")
        f.write("#define AIGATE_ADMIN_UI_HTML_H\n\n")
        f.write("#include <stddef.h>\n\n")
        f.write("static const unsigned char g_admin_ui_html[] = {\n")
        for i, b in enumerate(data):
            f.write(f"0x{b:02x}, ")
            if (i + 1) % 16 == 0:
                f.write("\n")
        f.write("0x00\n};\n\n")
        f.write(f"static const size_t g_admin_ui_html_len = {len(data)};\n\n")
        f.write("#endif /* AIGATE_ADMIN_UI_HTML_H */\n")

if __name__ == "__main__":
    main()
