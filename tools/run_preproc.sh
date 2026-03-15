#!/usr/bin/env bash
# Pre-process all C files that contain INCBIN_U* macros, expanding them to C data.
# Also pre-process .h files with INCBIN_U* (data definition headers included by .c
# files), outputting them to preproc_headers/ so the compiler picks them up first.
# Run by CMakeLists.txt at configure time.
#
# Strategy: do NOT use `cpp -E` (which expands all #includes and causes duplicate
# definitions when compiled with -include global.h). Instead, use a lightweight
# perl pass to strip the only construct that confuses preproc: inline C++ comments
# that appear after a quoted string inside multi-argument INCBIN calls.
#
# Usage: tools/run_preproc.sh <source_root> <output_dir>
# For each src/*.c containing INCBIN_U*, produces <output_dir>/<name>.preproc.c
# For each src/data/*.h containing INCBIN_U*, produces <output_dir>/../preproc_headers/<rel>.h

set -euo pipefail

SOURCE_ROOT="$1"
OUTPUT_DIR="$2"
HEADERS_DIR="$(dirname "$OUTPUT_DIR")/preproc_headers"

PREPROC="$SOURCE_ROOT/tools/preproc/preproc"
CHARMAP="$SOURCE_ROOT/charmap.txt"

if [ ! -x "$PREPROC" ]; then
    echo "ERROR: preproc not found at $PREPROC"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
mkdir -p "$HEADERS_DIR"

cd "$SOURCE_ROOT"

OK=0
FAIL=0
SKIP=0

# ---------------------------------------------------------------------------
# Helper: run preproc on a single file (with perl comment stripping)
# Usage: run_preproc_file <src> <out> <tmpbase>
# ---------------------------------------------------------------------------
run_preproc_file() {
    local src="$1" out="$2" tmpbase="$3"
    local stripped="${tmpbase}.stripped.c"  # must end in .c — preproc rejects other extensions

    # Skip if output is up-to-date
    if [ -f "$out" ] && [ "$out" -nt "$src" ]; then
        SKIP=$((SKIP+1))
        return 0
    fi

    # Strip inline // comments after quoted strings in INCBIN calls
    perl -pe 's/("[\w.\/-]+"),\s*\/\/[^\n]*$/$1,/g' "$src" > "$stripped"

    if "$PREPROC" "$stripped" "$CHARMAP" > "$out" 2>/dev/null; then
        OK=$((OK+1))
    else
        echo "FAIL: preproc failed for $src"
        cp "$src" "$out"
        FAIL=$((FAIL+1))
    fi

    rm -f "$stripped"
}

# ---------------------------------------------------------------------------
# 1. Process .c files
# ---------------------------------------------------------------------------
INCBIN_C_FILES=$(grep -rl "INCBIN_U" src/ --include="*.c" 2>/dev/null)

for src in $INCBIN_C_FILES; do
    name=$(basename "$src" .c)
    out="${OUTPUT_DIR}/${name}.preproc.c"
    run_preproc_file "$src" "$out" "${OUTPUT_DIR}/${name}"
done

echo "run_preproc (.c): $OK OK, $SKIP skipped, $FAIL failed"

OK=0; FAIL=0; SKIP=0

# ---------------------------------------------------------------------------
# 2. Process .h files — output preserving relative path under src/
#    e.g. src/data/graphics/intro_scene.h
#      -> build_vita/preproc_headers/data/graphics/intro_scene.h
# ---------------------------------------------------------------------------
INCBIN_H_FILES=$(grep -rl "INCBIN_U" src/ --include="*.h" 2>/dev/null)

for src in $INCBIN_H_FILES; do
    # Compute relative path from src/ (e.g. "data/graphics/intro_scene.h")
    rel="${src#src/}"
    out="${HEADERS_DIR}/${rel}"
    out_dir="$(dirname "$out")"
    mkdir -p "$out_dir"
    run_preproc_file "$src" "$out" "${HEADERS_DIR}/${rel%.h}"
done

echo "run_preproc (.h): $OK OK, $SKIP skipped, $FAIL failed"
