# Architecture — SDL2 → Vita

Document de référence pour le portage de la couche platform.
À mettre à jour après inspection du repo réel.

---

## Mapping des APIs

### Fenêtre / Rendu

| SDL2 | Vita (VitaSDK) | Notes |
|---|---|---|
| `SDL_Init(SDL_INIT_VIDEO)` | `vita2d_init()` | Init du renderer |
| `SDL_CreateWindow()` | — | Pas de fenêtre sur Vita, surface unique |
| `SDL_CreateRenderer()` | `vita2d_start_drawing()` | |
| `SDL_CreateTexture()` | `vita2d_create_empty_texture()` | |
| `SDL_UpdateTexture()` | Copie dans `vita2d_texture_get_datap()` | |
| `SDL_RenderCopy()` | `vita2d_draw_texture_scale()` | Permet le scaling |
| `SDL_RenderPresent()` | `vita2d_end_drawing()` + `vita2d_swap_buffers()` | |

### Audio (RÉEL — vérifié dans sdl2.c + music_player.c)

**SDL2 actuel** :
- `SDL_OpenAudio(&want, 0)` avec `freq=42048`, `format=AUDIO_F32`, `channels=2`, `samples=1024`
- **Pas de callback** — mode queue : `SDL_QueueAudio(1, audioBuffer, samplesPerFrame * 4)`
  appelé depuis `src/music_player.c` l.764
- `samplesPerFrame * 2` samples stéréo de float32 par frame
- CGB audio : `cgb_audio_init(42048)` + `cgb_audio_generate()` dans `src/platform/cgb_audio.c`

| SDL2 (actuel) | Vita (VitaSDK) | Notes |
|---|---|---|
| `SDL_OpenAudio()` 42048Hz F32 stéréo | `sceAudioOutOpenPort()` 48000Hz | Adapter le sample rate |
| `SDL_QueueAudio()` (push depuis jeu) | Thread dédié + `sceAudioOutOutput()` | Vita est pull |
| `SDL_PauseAudio(1)` pour speedup | `sceAudioOutSetVolume()` à 0 | |

> ⚠️ Le modèle SDL2 ici est **queue** (pas callback). Le jeu pousse les samples.
> Sur Vita, SceAudio est en mode **pull** (thread bloquant).
> Il faut un ring buffer intermédiaire : le jeu écrit, le thread SceAudio lit.

### Input

| SDL2 | Vita (VitaSDK) | Notes |
|---|---|---|
| `SDL_PollEvent(SDL_KEYDOWN)` | `sceCtrlPeekBufferPositive()` | |
| `SDLK_z` (bouton A) | `SCE_CTRL_CROSS` | |
| `SDLK_x` (bouton B) | `SCE_CTRL_CIRCLE` | |
| `SDLK_RETURN` (Start) | `SCE_CTRL_START` | |
| `SDLK_RSHIFT` (Select) | `SCE_CTRL_SELECT` | |
| `SDLK_UP/DOWN/LEFT/RIGHT` | `SCE_CTRL_UP/DOWN/LEFT/RIGHT` | |
| `SDLK_a` (L) | `SCE_CTRL_LTRIGGER` | |
| `SDLK_s` (R) | `SCE_CTRL_RTRIGGER` | |

### Temps / Timers

| SDL2 | Vita (VitaSDK) | Notes |
|---|---|---|
| `SDL_GetTicks()` | `sceKernelGetProcessTimeWide()` | En microsecondes sur Vita |
| `SDL_Delay()` | `sceKernelDelayThread()` | En microsecondes |

### Fichiers / Saves

| SDL2 | Vita (VitaSDK) | Notes |
|---|---|---|
| `fopen/fclose` standard | `SceIo` ou libc standard | La libc Vita supporte fopen |
| Chemin save : `./` | `ux0:data/pokeemerald/` | Chemin standard homebrew Vita |

---

## Framebuffer pokeemerald (RÉEL — vérifié dans sdl2.c)

- Format : **ABGR1555** (`SDL_PIXELFORMAT_ABGR1555`) — 16-bit, RGB555 + 1 bit alpha
  - `SDL_CreateTexture(..., SDL_PIXELFORMAT_ABGR1555, ..., 240, 160)`
  - Buffer : `uint16_t image[DISPLAY_WIDTH * DISPLAY_HEIGHT]` (240×160 = 38400 px)
  - Pitch : `DISPLAY_WIDTH * sizeof(Uint16)` = 480 octets par ligne
- Dimensions source : **240×160** (`DISPLAY_WIDTH`/`DISPLAY_HEIGHT` dans `include/gba/defines.h`)
- Dimensions cible Vita : **960×544** (OLED natif)

**Pipeline de rendu sdl2.c** :
1. `VBlankIntrWait()` → signal atomique `isFrameAvailable = 1` + attend sémaphore
2. Thread principal : `VDraw()` → `DrawFrame()` (rendu scanline software) → `SDL_UpdateTexture()`
3. `SDL_RenderCopy()` + `SDL_RenderPresent()`
4. Post-signal : `SDL_SemPost(vBlankSemaphore)` pour libérer le thread jeu

**Pour Vita** : remplacer `SDL_UpdateTexture` par copie dans `vita2d_texture_get_datap()`
puis `vita2d_draw_texture_scale()`.

**Format ABGR1555 → vita2d** : vita2d supporte `SCE_GXM_TEXTURE_FORMAT_A1R5G5B5`,
il faudra peut-être swapper R et B (dépend du hardware).

Pour le viewport étendu (Phase 6) :
- Tiles GBA : **8×8 pixels** → à 32px/tile : ~30×17 tiles en 960×544
- `DISPLAY_WIDTH` → 480 (ou 960), `DISPLAY_HEIGHT` → 272 (ou 544)
- Constantes dans `include/gba/defines.h` l.94-99

---

## Structure des fichiers vita/ à créer

```c
// vita/main.c
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

int main() {
    vita2d_init();
    // init audio, input
    // game loop
    sceKernelExitProcess(0);
    return 0;
}

// vita/video.c
// vita2d_texture* framebuffer_texture;
// void vita_video_init(int width, int height);
// void vita_video_present(void* pixels, int pitch);

// vita/audio.c
// Ring buffer + thread SceAudio

// vita/input.c
// uint32_t vita_get_buttons(); // retourne un masque GBA-compatible
```

---

## Notes importantes

- La Vita n'a **pas de bouton Home** accessible en homebrew — utiliser `Start+Select` pour quitter
- La mémoire disponible : ~256MB RAM utilisateur
- La résolution native est **960×544** — ne jamais forcer 480×272 (mode PSP) pour ce projet
- vita2d gère le double buffering automatiquement avec `vita2d_swap_buffers()`