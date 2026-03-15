/*
 * vita_stubs.c — No-op stubs for symbols that are unavailable on PS Vita.
 *
 * Three categories:
 *  1. SceSharedFb / SceAppMgr  — used internally by vita2d, not in homebrew SDK
 *  2. MP2K audio engine        — GBA m4a_1.s is not compiled; Phase 1 = silence
 *  3. voicegroup000            — sound table entry referenced by m4a_tables.c
 */

#include <stdint.h>

/* -------------------------------------------------------------------------
 * 1. vita2d internal SDK stubs
 *    vita2d references these SceSharedFb / SceAppMgr functions that are
 *    absent from the homebrew VitaSDK stub libraries.
 * ---------------------------------------------------------------------- */

/* SceAppMgr */
int sceAppMgrGetBudgetInfo(void *info) { (void)info; return 0; }

/* SceSharedFb */
int _sceSharedFbOpen(int id, int unk)    { (void)id; (void)unk; return 0; }
int sceSharedFbClose(int handle)          { (void)handle; return 0; }
int sceSharedFbGetInfo(int handle, void *info) { (void)handle; (void)info; return 0; }
int sceSharedFbBegin(int handle, void *buf)    { (void)handle; (void)buf; return 0; }
int sceSharedFbEnd(int handle)                 { (void)handle; return 0; }

/* -------------------------------------------------------------------------
 * 2. MP2K / M4A audio engine stubs
 *    The GBA m4a_1.s is not assembled for Vita; music_player.c is commented
 *    out. Provide empty stubs so the linker is satisfied.
 *    Audio will be silent until Phase 4 (SceAudio ring buffer).
 * ---------------------------------------------------------------------- */

#include "gba/m4a_internal.h"

void MP2KClearChain(struct MixerSource *chan) { (void)chan; }

void MP2K_event_fine(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_goto(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_patt(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_pend(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_rept(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_prio(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_tempo(struct MP2KPlayerState *p, struct MP2KTrack *t)  { (void)p; (void)t; }
void MP2K_event_keysh(struct MP2KPlayerState *p, struct MP2KTrack *t)  { (void)p; (void)t; }
void MP2K_event_voice(struct MP2KPlayerState *p, struct MP2KTrack *t)  { (void)p; (void)t; }
void MP2K_event_vol(struct MP2KPlayerState *p, struct MP2KTrack *t)    { (void)p; (void)t; }
void MP2K_event_pan(struct MP2KPlayerState *p, struct MP2KTrack *t)    { (void)p; (void)t; }
void MP2K_event_bend(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_bendr(struct MP2KPlayerState *p, struct MP2KTrack *t)  { (void)p; (void)t; }
void MP2K_event_lfos(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_lfodl(struct MP2KPlayerState *p, struct MP2KTrack *t)  { (void)p; (void)t; }
void MP2K_event_mod(struct MP2KPlayerState *p, struct MP2KTrack *t)    { (void)p; (void)t; }
void MP2K_event_modt(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_tune(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_port(struct MP2KPlayerState *p, struct MP2KTrack *t)   { (void)p; (void)t; }
void MP2K_event_endtie(struct MP2KPlayerState *p, struct MP2KTrack *t) { (void)p; (void)t; }
/* nxx variant (note-on with extra params) */
void MP2K_event_nxx(struct MP2KPlayerState *p, struct MP2KTrack *t)    { (void)p; (void)t; }

/* -------------------------------------------------------------------------
 * 3. voicegroup000
 *    Referenced as a ToneData entry in m4a_tables.c. Stub with zeroed data.
 * ---------------------------------------------------------------------- */

const struct ToneData voicegroup000 = {0};
