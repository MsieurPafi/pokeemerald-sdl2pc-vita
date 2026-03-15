# pokeemerald-vita

Port natif de Pokémon Émeraude sur PS Vita, basé sur `NTx86/pokeemerald-sdl2pc` (branche `wii_port`).

**Objectif final :** viewport étendu natif — le joueur voit plus de tiles autour de lui
(pas un simple scale), rendu nativement sur l'écran OLED 960×544 de la Vita.

---

## État d'avancement

| Phase | Description | État |
|-------|-------------|------|
| 1 | Compilation + link + VPK installable (données zeroed, audio silencieux) | ✅ **Terminé** |
| 2 | Rendu réel via vita2d (framebuffer GBA → texture Vita) | ⬜ À faire |
| 3 | Input SceCtrl (boutons Vita → masque GBA) | ⬜ À faire |
| 4 | Audio SceAudio ring buffer (remplace stub SDL_QueueAudio) | ⬜ À faire |
| 5 | Saves (SceIo → ux0:data/PKEMRLD00/) | ⬜ À faire |
| 6 | Viewport étendu (SCREEN_TILE_WIDTH/HEIGHT + caméra) | ⬜ À faire |

---

## Architecture du projet

```
pokeemerald-sdl2pc/
├── src/
│   ├── platform/
│   │   ├── sdl2.c              ← PORT WII (2499 lignes) — EXCLU du build Vita
│   │   ├── cgb_audio.c         ← Émulation canaux CGB (compilé pour Vita)
│   │   └── vita/
│   │       ├── vita.c          ← Platform layer Vita (Phase 1 : stubs minimalistes)
│   │       └── vita_stubs.c    ← Stubs vita2d internes + MP2K audio + voicegroup
│   └── ...                     ← Logique jeu pokeemerald (NE PAS MODIFIER)
├── gflib/                      ← Bibliothèque graphique GBA (compilée pour Vita)
├── data/
│   ├── *.s                     ← Scripts de bataille, maps, events, sons (assemblés via vita_asm.sh)
│   ├── specials_indices.inc    ← Généré : .set SPECIAL_Func, N  (évite R_ARM_ABS16)
│   ├── maps/
│   │   ├── map_groups.json     ← Source
│   │   ├── groups.inc          ← Généré par mapjson
│   │   ├── headers.inc         ← Généré par mapjson
│   │   ├── connections.inc     ← Généré par mapjson
│   │   ├── events.inc          ← Généré par mapjson
│   │   └── */header.inc        ← Généré par mapjson (518 maps)
│   └── layouts/
│       ├── layouts.json        ← Source
│       ├── layouts.inc         ← Généré par mapjson
│       └── layouts_table.inc   ← Généré par mapjson
├── include/
│   ├── global.h                ← Modifié : blocs #ifdef __vita__ pour INCBIN + charmap stubs
│   ├── gba/types.h             ← u8/u16/u32/etc.
│   └── constants/
│       └── specials.h          ← Généré : #define SPECIAL_Func N (non utilisé pour l'asm)
├── tools/
│   ├── vita_asm.sh             ← Pipeline : preproc → gcc -E → sed → arm-vita-eabi-as
│   ├── vita_expand_includes.py ← (obsolète, remplacé par preproc)
│   ├── preproc/preproc         ← Outil natif macOS (compilé) — gère .include, .string, .braille, ::
│   ├── jsonproc/jsonproc       ← Outil natif macOS (compilé) — génère wild_encounters.h, region_map_entries.h
│   └── mapjson/mapjson         ← Outil natif macOS (compilé) — génère les .inc de maps
├── src/data/
│   ├── region_map/region_map_entries.h  ← Généré par jsonproc
│   └── wild_encounters.h               ← Généré par jsonproc
├── CMakeLists.txt              ← Build Vita (VitaSDK)
├── charmap.txt                 ← Table d'encodage texte Pokémon (requis par preproc)
└── CLAUDE.md
```

---

## Build Vita — Prérequis

1. **VitaSDK** installé dans `/usr/local/vitasdk`
2. **Outils natifs macOS compilés** (à faire une seule fois) :
   ```bash
   # preproc (gère .include, .string, .braille, :: dans les .s)
   cd tools/preproc
   c++ -stdlib=libc++ -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1 \
       -std=c++11 -O2 -Wall -Wno-switch \
       asm_file.cpp c_file.cpp charmap.cpp preproc.cpp string_parser.cpp utf8.cpp -o preproc

   # jsonproc (génère region_map_entries.h et wild_encounters.h)
   cd tools/jsonproc
   clang++ -stdlib=libc++ -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1 \
       -I. -Wall -std=c++17 -O2 jsonproc.cpp -o jsonproc

   # mapjson (déjà compilé — tools/mapjson/mapjson)
   ```

3. **Fichiers générés** (à regénérer si les JSON sources changent) :
   ```bash
   # Données de region map et wild encounters
   tools/jsonproc/jsonproc src/data/region_map/region_map_sections.json \
       src/data/region_map/region_map_sections.json.txt \
       src/data/region_map/region_map_entries.h
   tools/jsonproc/jsonproc src/data/wild_encounters.json \
       src/data/wild_encounters.json.txt \
       src/data/wild_encounters.h

   # Données de maps (layouts + 518 maps individuelles)
   tools/mapjson/mapjson layouts emerald data/layouts/layouts.json
   tools/mapjson/mapjson groups  emerald data/maps/map_groups.json
   find data/maps -name 'map.json' | while read f; do
       tools/mapjson/mapjson map emerald "$f" data/layouts/layouts.json
   done

   # Index des specials (évite R_ARM_ABS16 dans les scripts)
   grep -E '^\s+def_special ' data/specials.inc \
       | sed 's/.*def_special[[:space:]]*//' \
       | awk '{printf "\t.set SPECIAL_%s, %d\n", $1, NR-1}' \
       > data/specials_indices.inc
   ```

---

## Build Vita — Compilation

```bash
VITASDK=/usr/local/vitasdk cmake -B build_vita
make -C build_vita -j$(sysctl -n hw.logicalcpu)
# Produit : build_vita/pokeemerald.vpk  (≈ 2.8 MB)
```

---

## Pipeline d'assemblage `data/*.s`

Les 15 fichiers `data/*.s` (scripts de bataille, maps, events, sons...) utilisent
des extensions propriétaires du build GBA. Notre pipeline les traduit pour `arm-vita-eabi-as` :

```
tools/preproc/preproc $INPUT charmap.txt
  → expand .include récursif (toute l'arborescence data/, asm/, constants/, sound/)
  → encode .string "text" → .byte N, N, N, ...  (encodage texte Pokémon)
  → encode .braille "ABC$" → .byte N, N, N, ...  (encodage braille)
  → convertit label:: → label: ; .global label
  |
arm-vita-eabi-gcc -E -x assembler-with-cpp -I include -I . -D__VITA__=1 -
  → expand #include/#define C (FOREACH_TM, constantes de chansons/maps, etc.)
  |
sed 's/\.4byte/\.int/g;s/\.2byte/\.short/g'
  → renomme les pseudo-ops GAS non supportés
  |
arm-vita-eabi-as -march=armv7-a -mthumb
  → produit l'objet ELF ARM
```

**Point clé :** `preproc` doit s'exécuter avec `CWD=$SRCROOT` pour que les chemins
`.include "asm/macros.inc"` se résolvent correctement.

---

## Ce qui est stubbé en Phase 1

| Symbole/Feature | Stub | Fichier |
|-----------------|------|---------|
| `INCBIN_U8/U16/U32/S8/S16/S32` | `{0}` (données zeroed) | `include/global.h` |
| `_()` / `__()` macros charmap | `{x}` | `include/global.h` |
| `MP2K_event_*` (21 fonctions) | no-op | `vita_stubs.c` |
| `voicegroup000` | `{0}` | `vita_stubs.c` |
| `sceSharedFb*` / `sceAppMgrGetBudgetInfo` | no-op / 0 | `vita_stubs.c` |
| Rendu vidéo | vide | `vita.c` |
| Input | tous boutons relâchés | `vita.c` |
| Audio | silencieux | `vita.c` |
| Save | silencieux | `vita.c` |

---

## Constantes de compilation

```cmake
PORTABLE=1       # utilise les extern vars au lieu des adresses mémoire GBA
NONMATCHING=1    # désactive les assertions de matching binaire
UBFIX=1          # active les corrections UB (undefined behaviour)
MODERN=0         # désactive le macro abs() qui conflicte avec stdlib VitaSDK
```

---

## Règles pour Claude Code

- **Ne jamais modifier** `src/` (logique jeu) sauf constantes viewport (Phase 6)
- **Ne jamais modifier** les `.s` dans `data/` sauf ajout de `#include` en tête ou `.include "data/specials_indices.inc"`
- Tout le code Vita va dans `src/platform/vita/`
- `vita_stubs.c` : ajouter les stubs en Phase 1, remplacer par le vrai code aux phases suivantes
- Les fichiers générés (`data/maps/*/header.inc`, `data/layouts/*.inc`, etc.) ne sont pas committés — les regénérer au besoin

---

## Phases suivantes — notes techniques

### Phase 2 — Rendu vita2d
- `VDraw()` dans `sdl2.c` : la framebuffer GBA (240×160, palette 15bpp) est rendue scanline par scanline
- Vita : créer une texture `vita2d_texture` 240×160, la mettre à jour chaque frame, la scaler à 960×544 (×4) ou laisser le viewport étendu faire le travail (Phase 6)
- `gflib/gpu_regs.c` : les registres GPU GBA sont emulés en RAM dans `gPortIOBuf`

### Phase 4 — Audio SceAudio
- `music_player.c` contient `MP2KPlayerMain()` qui appelle `SDL_QueueAudio()`
- Remplacer par un ring buffer SceAudio : thread dédié + `sceAudioOutOutput()`
- La synthèse audio MP2K est déjà dans `music_player.c` — seule la sortie change

### Phase 6 — Viewport étendu
```c
// include/gba/defines.h (ou overworld.h)
#define SCREEN_TILE_WIDTH   15   // → 30 (960 / 32)
#define SCREEN_TILE_HEIGHT  10   // → 17 (544 / 32)
#define DISPLAY_WIDTH       240  // → 480 ou 960
#define DISPLAY_HEIGHT      160  // → 272 ou 544
```
