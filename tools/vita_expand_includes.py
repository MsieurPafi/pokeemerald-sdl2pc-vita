#!/usr/bin/env python3
"""
vita_expand_includes.py — recursively expand GAS .include directives inline.

Usage: python3 vita_expand_includes.py <input.s> <srcroot>

Reads <input.s>, replaces every  .include "path"  line with the full content
of that file (resolved relative to <srcroot>), recursively.  The result is
written to stdout.  C preprocessor directives (#include, #ifndef, …) are left
intact for arm-vita-eabi-gcc -E to handle.

This is needed because arm-vita-eabi-as processes .include files directly from
disk, bypassing our :: → .global sed step.  By expanding them here, the entire
assembly source is a single flat stream that sed can sanitise before gas sees it.
"""

import re
import sys
import os

DOT_INCLUDE = re.compile(r'^\s*\.include\s+"([^"]+)"')

def expand(path, srcroot, depth=0):
    if depth > 50:
        sys.stderr.write(f"vita_expand_includes: depth limit reached at {path}\n")
        return
    try:
        with open(path, 'r', errors='replace') as fh:
            for line in fh:
                m = DOT_INCLUDE.match(line)
                if m:
                    inc_rel = m.group(1)
                    # Try relative to srcroot first, then relative to the
                    # directory of the current file.
                    candidates = [
                        os.path.join(srcroot, inc_rel),
                        os.path.join(os.path.dirname(path), inc_rel),
                    ]
                    found = None
                    for c in candidates:
                        if os.path.isfile(c):
                            found = os.path.realpath(c)
                            break
                    if found:
                        expand(found, srcroot, depth + 1)
                    else:
                        # Can't resolve — emit as a #include so gcc -E tries
                        sys.stdout.write(f'#include "{inc_rel}"\n')
                else:
                    sys.stdout.write(line)
    except FileNotFoundError:
        sys.stderr.write(f"vita_expand_includes: not found: {path}\n")

if __name__ == '__main__':
    input_file = sys.argv[1]
    srcroot    = sys.argv[2]
    expand(os.path.realpath(input_file), srcroot)
