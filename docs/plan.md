# Plan — pokeemerald-vita

Roadmap du port PS Vita de pokeemerald.
Claude Code peut cocher les cases au fil de l'avancement.

---

## Phase 0 — Setup & exploration

- [ ] Cloner `NTx86/pokeemerald-sdl2pc` et créer une branche `vita-port`
- [ ] Compiler la version SDL2 sur macOS (vérifier que ça tourne)
- [ ] Cartographier les fichiers de la couche platform SDL2 (mettre à jour `CLAUDE.md`)
- [ ] Identifier les constantes de viewport (`SCREEN_TILE_WIDTH` etc.) et leurs fichiers
- [ ] Identifier les dépendances SDL2 dans le CMakeLists.txt
- [ ] Créer le dossier `src/platform/vita/`

---

## Phase 1 — Build system Vita

- [ ] Ajouter le toolchain VitaSDK au CMakeLists.txt (`-DPLATFORM=vita`)
- [ ] Créer un `CMakeLists.txt` ou `Makefile` dédié Vita
- [ ] Vérifier que le code jeu (`src/`) compile sans erreur avec `arm-vita-eabi-gcc`
- [ ] Produire un `.elf` minimal (même vide/crashant) pour valider la chaîne de build

---

## Phase 2 — Couche platform Vita (stub)

Créer des stubs vides pour chaque composant, afin d'obtenir un binaire qui démarre.

- [ ] `vita/main.c` — entry point `int main()`, init SceKernel, boucle vide
- [ ] `vita/video.c` — init vita2d, présente un écran noir
- [ ] `vita/audio.c` — init SceAudio, silence
- [ ] `vita/input.c` — lit SceCtrl, ne fait rien encore
- [ ] Le jeu démarre et affiche quelque chose sur la Vita

---

## Phase 3 — Rendu (video.c)

- [ ] Comprendre le format du framebuffer produit par pokeemerald (RGB565 ? RGBA8888 ?)
- [ ] Créer une texture vita2d à partir du framebuffer
- [ ] Afficher la texture à l'écran (240×160 centré d'abord)
- [ ] Le jeu est visuellement fonctionnel (image correcte, pas de corruption)
- [ ] Scaler proprement pour remplir 960×544 (étape intermédiaire)

---

## Phase 4 — Input (input.c)

- [ ] Mapper les boutons GBA → boutons Vita :
  - A → Croix, B → Rond, Start → Start, Select → Select
  - D-Pad → D-Pad
  - L/R → L/R
- [ ] Le jeu répond aux entrées correctement

---

## Phase 5 — Audio (audio.c)

- [ ] Comprendre le format audio produit (sample rate, canaux, format)
- [ ] Ouvrir un thread SceAudio
- [ ] Alimenter le buffer audio depuis la sortie du jeu
- [ ] Son fonctionnel en jeu

---

## Phase 6 — Viewport étendu ⭐

C'est la feature principale, celle qui justifie l'existence du projet.

- [ ] Localiser les constantes de viewport dans le code
- [ ] Comprendre comment la caméra charge les tiles autour du joueur
- [ ] Modifier `SCREEN_TILE_WIDTH` et `SCREEN_TILE_HEIGHT` pour 960×544
- [ ] Adapter le renderer vita pour afficher la surface étendue
- [ ] Vérifier qu'il n'y a pas d'artefacts aux bords de la map (tiles manquants)
- [ ] Tester dans plusieurs zones (ville, route, intérieur)

---

## Phase 7 — Polish & QoL

- [ ] Savestates natifs (via SceAppUtil ou fichier custom)
- [ ] 60fps stables (profiler si besoin)
- [ ] Écran de chargement/splash screen style Vita
- [ ] Gestion correcte de la mise en veille (SceDisplay suspend)
- [ ] Packaging `.vpk` propre avec icône et metadata

---

## Backlog / idées futures

- Support pokeemerald-expansion (features Gen 4/5/6)
- Multijoueur local via WiFi Vita (SceNet)
- Interface de debug overlay (FPS, tile coords)
- Port d'autres décompilations pret (pokefirered, etc.)

---

## Notes de debug

*(Claude Code remplit cette section au fur et à mesure)*

- ...