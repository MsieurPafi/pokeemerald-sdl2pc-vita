#!/usr/bin/env python3
"""
Fix GBA title screen tilesets: convert 16-tile-wide .4bpp to 32-tile-wide format.

The tilemap .bin files (rayquaza.bin, clouds.bin) use tile indices for a 32-tile-wide
tileset (256px wide), but gbagfx generates .4bpp from 128px-wide PNGs (16-tile-wide).
This script remaps tile data to the correct 32-wide positions so the renderer uses
the right pixel data for each tile index.

Usage:
    python3 tools/fix_title_tilesets.py <source_root>

Idempotent: skips files that are already in 32-wide format (32768 bytes = 1024 tiles).
Returns exit code 0 if any file was modified, 1 if all were already up-to-date.
"""

import sys
import os

TILE_SIZE   = 32   # 4bpp: 8x8 pixels × (4 bits/pixel) = 32 bytes
TILES_32W   = 1024 # 32 cols × 32 rows
BYTES_32W   = TILES_32W * TILE_SIZE  # 32768 bytes
ZERO_TILE   = bytes(TILE_SIZE)


def read_tiles(path):
    with open(path, 'rb') as f:
        data = f.read()
    n = len(data) // TILE_SIZE
    return [data[i * TILE_SIZE:(i + 1) * TILE_SIZE] for i in range(n)]


def write_tiles(path, tiles):
    with open(path, 'wb') as f:
        for tile in tiles:
            f.write(tile)


def get_tile(tiles_16, r16, c16):
    """Safely get tile from 16-wide source; returns ZERO_TILE if out of range."""
    idx = r16 * 16 + c16
    if idx < len(tiles_16):
        return tiles_16[idx]
    return ZERO_TILE


def fix_rayquaza(source_root):
    """
    Convert rayquaza.4bpp from 16-wide (256 tiles, 8192 bytes) to
    32-wide (1024 tiles, 32768 bytes).

    The sky gradient spans rows 0-15 of the 16-wide tileset.  For seamless
    vertical scrolling the BG wraps every 256px (32 tilemap rows), so:
      32-wide row 23  ==  32-wide row  7  (= 16-wide row  7)
      32-wide row 31  ==  32-wide row 15  (= 16-wide row 15)
    """
    path = os.path.join(source_root, 'graphics', 'title_screen', 'rayquaza.4bpp')
    if not os.path.exists(path):
        print(f'SKIP rayquaza.4bpp: not found at {path}')
        return False

    tiles_16 = read_tiles(path)
    if len(tiles_16) == TILES_32W:
        print(f'SKIP rayquaza.4bpp: already 32-wide ({len(tiles_16)} tiles)')
        return False

    # Rows that duplicate other rows for seamless vertical scroll
    row_dup = {23: 7, 31: 15}

    out = []
    for r32 in range(32):
        r16 = row_dup.get(r32, r32)   # resolve duplication
        for c32 in range(32):
            if c32 >= 16:
                out.append(ZERO_TILE)  # right half of tileset unused
            else:
                out.append(get_tile(tiles_16, r16, c32))

    write_tiles(path, out)
    print(f'FIXED rayquaza.4bpp: {len(tiles_16)} tiles -> {len(out)} tiles (32-wide)')
    return True


def fix_clouds(source_root):
    """
    Clouds.4bpp MUST NOT be converted to 32-wide (32768 bytes).

    VRAM layout constraint: clouds.4bpp is loaded at BG_CHAR_ADDR(3) =
    VRAM[0xC000].  The rayquaza tilemap lives at BG_SCREEN_ADDR(26) =
    VRAM[0xD000..0xD7FF], which is only 4096 bytes from the charbase 3 start.
    A 32-wide clouds.4bpp (32768 bytes) would extend from VRAM[0xC000] to
    VRAM[0x13FFF], stomping over the rayquaza tilemap at 0xD000.

    Therefore we leave clouds.4bpp at its original size (≤ 4096 bytes).  All
    tile indices referenced by clouds.bin (224, 228, 736, …) fall in the
    VRAM[0xDC00+] range which gets overwritten by the clouds tilemap anyway,
    making them transparent.  The visual result is an invisible-but-harmless
    clouds layer that reveals the sky backdrop behind it.
    """
    path = os.path.join(source_root, 'graphics', 'title_screen', 'clouds.4bpp')
    if not os.path.exists(path):
        print(f'SKIP clouds.4bpp: not found at {path}')
        return False

    tiles = read_tiles(path)
    size  = len(tiles) * TILE_SIZE
    # 128 tiles = 4096 bytes is the safe maximum that doesn't reach 0xD000.
    if size > 4096:
        # Truncate to safe size (keep the first 128 tiles only).
        write_tiles(path, tiles[:128])
        print(f'FIXED clouds.4bpp: truncated {len(tiles)} tiles -> 128 tiles '
              f'(avoids VRAM[0xD000] conflict)')
        return True

    print(f'SKIP clouds.4bpp: {len(tiles)} tiles ({size} bytes) — already safe size')
    return False


if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    modified = False
    modified |= fix_rayquaza(root)
    modified |= fix_clouds(root)
    sys.exit(0 if modified else 1)
