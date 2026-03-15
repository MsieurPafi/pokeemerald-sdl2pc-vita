#!/usr/bin/env bash
# Generate all graphics asset files (.lz and uncompressed) needed by the Vita/PC build.
# Requires: tools/gbagfx/gbagfx  (run 'make' in tools/gbagfx/ first)
#
# Usage: ./tools/generate_assets.sh [source_root]
#   source_root defaults to the parent of this script's directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${1:-"$(dirname "$SCRIPT_DIR")"}"
GFX="$SCRIPT_DIR/gbagfx/gbagfx"

if [ ! -x "$GFX" ]; then
    echo "ERROR: gbagfx not found at $GFX. Run 'make' in tools/gbagfx/ first."
    exit 1
fi

echo "Generating assets in: $ROOT"
cd "$ROOT"

OK=0
FAIL=0
SKIP=0

# Generate one file from source(s). Tries PNG, PNG_0, .pal as sources.
# Usage: generate_binary TARGET_FILE
generate_binary() {
    local target="$1"
    local ext="${target##*.}"
    local base_noext="${target%.*}"

    [ -f "$target" ] && { SKIP=$((SKIP+1)); return 0; }

    case "$ext" in
        4bpp|8bpp|1bpp|latfont|hwjpnfont|fwjpnfont)
            for src in "${base_noext}.png" "${base_noext}_0.png"; do
                if [ -f "$src" ]; then
                    if "$GFX" "$src" "$target" 2>/dev/null; then
                        OK=$((OK+1)); return 0
                    else
                        echo "FAIL: $src -> $target"; FAIL=$((FAIL+1)); return 1
                    fi
                fi
            done
            ;;
        gbapal)
            for src in "${base_noext}.pal" "${base_noext}.png"; do
                if [ -f "$src" ]; then
                    if "$GFX" "$src" "$target" 2>/dev/null; then
                        OK=$((OK+1)); return 0
                    else
                        echo "FAIL: $src -> $target"; FAIL=$((FAIL+1)); return 1
                    fi
                fi
            done
            ;;
        lz|rl)
            # Strip the compression suffix and ensure the intermediate exists, then compress
            local base_comp="${target%.*}"
            generate_binary "$base_comp" 2>/dev/null || true
            if [ -f "$base_comp" ]; then
                if "$GFX" "$base_comp" "$target" 2>/dev/null; then
                    OK=$((OK+1)); return 0
                else
                    echo "FAIL: $base_comp -> $target"; FAIL=$((FAIL+1)); return 1
                fi
            fi
            ;;
    esac

    echo "MISSING source for: $target"
    FAIL=$((FAIL+1))
    return 1
}

# --- Pass 1: .lz files ---
ALL_LZ=$(grep -roh '"[^"]*\.lz"' src/ --include="*.c" --include="*.h" 2>/dev/null \
       | tr -d '"' | sort -u)

for f in $ALL_LZ; do
    generate_binary "$f" || true
done

# --- Pass 2: non-compressed binary files (.gbapal, .4bpp, .8bpp, .bin) in graphics/ and data/ ---
ALL_BIN=$(grep -roh '"[^"]*\.\(gbapal\|4bpp\|8bpp\|1bpp\)"' \
              src/ --include="*.c" --include="*.h" 2>/dev/null \
         | tr -d '"' | grep '^\(graphics/\|data/\)' | sort -u)

for f in $ALL_BIN; do
    generate_binary "$f" || true
done

# --- Pass 3: .lz and .rl files under data/ ---
ALL_DATA_COMP=$(grep -roh '"[^"]*\.\(lz\|rl\)"' src/ --include="*.c" --include="*.h" 2>/dev/null \
              | tr -d '"' | grep '^\(data/\)' | sort -u)

for f in $ALL_DATA_COMP; do
    generate_binary "$f" || true
done

# --- Pass 4: font files (.latfont, .hwjpnfont, .fwjpnfont) ---
ALL_FONTS=$(grep -roh '"[^"]*\.\(latfont\|hwjpnfont\|fwjpnfont\)"' src/ --include="*.c" --include="*.h" 2>/dev/null \
          | tr -d '"' | sort -u)

for f in $ALL_FONTS; do
    generate_binary "$f" || true
done

# --- Pass 5: .rl files (run-length compressed) ---
ALL_RL=$(grep -roh '"[^"]*\.rl"' src/ --include="*.c" --include="*.h" 2>/dev/null \
       | tr -d '"' | grep '^\(graphics/\)' | sort -u)

for f in $ALL_RL; do
    generate_binary "$f" || true
done

echo ""
echo "Done: $OK generated, $SKIP already existed, $FAIL failed/missing"

# ---------------------------------------------------------------------------
# Post-process: fix title screen tilesets to 32-tile-wide layout.
#
# The tilemap .bin files (rayquaza.bin, clouds.bin) reference tile indices for
# a 32-wide (256px) tileset, but gbagfx generates .4bpp from 128px-wide PNGs
# (16-wide layout).  fix_title_tilesets.py remaps tile data in-place and
# returns exit code 0 only when it actually modified a file.  We then force-
# recompress the affected .lz files so the build picks up the fixed data.
# ---------------------------------------------------------------------------
FIX_SCRIPT="$SCRIPT_DIR/fix_title_tilesets.py"
if command -v python3 >/dev/null 2>&1 && [ -f "$FIX_SCRIPT" ]; then
    if python3 "$FIX_SCRIPT" "$ROOT"; then
        echo "Recompressing fixed title tilesets..."
        for tileset in rayquaza clouds; do
            bpp="$ROOT/graphics/title_screen/${tileset}.4bpp"
            lz="${bpp}.lz"
            if [ -f "$bpp" ]; then
                "$GFX" "$bpp" "$lz" 2>/dev/null \
                    && echo "  OK: ${tileset}.4bpp.lz" \
                    || echo "  FAIL: ${tileset}.4bpp.lz"
            fi
        done
    else
        echo "Title tilesets already 32-wide, skipping recompress."
    fi
else
    echo "WARNING: python3 or fix_title_tilesets.py not found; title tilesets not fixed."
fi
