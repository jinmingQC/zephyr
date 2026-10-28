#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unquote archive glob selectors in generated Zephyr app-memory scripts for LLD.

Do not run on source trees. CMake appends this to the existing generator
commands, so their outputs and dependencies remain owned by Zephyr.
"""
from pathlib import Path
import re
import sys

def fix(text):
    pattern = re.compile(r'"([^"\n]*[.]a:[^"\n]*)"(?=\s*\()')
    def selector(match):
        value = match.group(1)
        if re.search(r"\s", value):
            raise ValueError("Unsupported whitespace in an archive selector: " + value)
        return value
    return pattern.sub(selector, text)

if __name__ == "__main__":
    path = Path(sys.argv[1])
    original = path.read_text()
    updated = fix(original)
    if updated != original:
        path.write_text(updated)
