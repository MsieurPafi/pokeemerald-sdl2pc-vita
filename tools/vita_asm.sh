#!/bin/bash
# vita_asm.sh — assemble a pokeemerald .s data file for the PS Vita
#
# Usage: vita_asm.sh <input.s> <output.o> <srcroot> <include_dir>
#
# Mirrors the original Makefile pipeline:
#   $(PREPROC) $< charmap.txt | $(CPP) -I include | $(ASM_PSEUDO_OP_CONV) | $(AS) …
#
#   1. preproc      expand .include recursively; encode .string/.braille;
#                   convert label:: → label: ; .global label
#   2. gcc -E       expand C macros (FOREACH_TM, #ifdef guards, etc.)
#   3. sed          .4byte→.int  .2byte→.short  (GNU AS pseudo-op names)
#   4. arm-vita-eabi-as  produce ELF32 ARM object

set -e
INPUT="$1"
OUTPUT="$2"
SRCROOT="$3"
INCLUDEDIR="$4"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREPROC="$SCRIPT_DIR/preproc/preproc"
CHARMAP="$SRCROOT/charmap.txt"

# preproc resolves .include paths relative to CWD — must be the source root
cd "$SRCROOT"

"$PREPROC" "$INPUT" "$CHARMAP" \
  | arm-vita-eabi-gcc \
        -E \
        -x assembler-with-cpp \
        -I"$INCLUDEDIR" \
        -I"$SRCROOT" \
        -D__VITA__=1 \
        - 2>/dev/null \
  | sed -e 's/\.4byte/\.int/g;s/\.2byte/\.short/g' \
  | arm-vita-eabi-as \
        -march=armv7-a \
        -mthumb \
        -I"$SRCROOT" \
        -o "$OUTPUT" \
        -
