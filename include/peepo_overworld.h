#ifndef GUARD_PEEPO_OVERWORLD_H
#define GUARD_PEEPO_OVERWORLD_H

#include "global.h"

// Remote display-name buffer: up to PEEPO_NAME_LEN-1 charset glyphs plus EOS.
#define PEEPO_NAME_LEN 12

// Copy a peer-supplied display name into `dst` (a PEEPO_NAME_LEN buffer), keeping
// only printable glyphs. Bytes >= CHAR_DYNAMIC (0xF7) are text-engine control codes,
// not glyphs, and must never reach StringExpandPlaceholders (they would recurse or
// stall it). `src` is up to `srcLen` raw bytes, terminated early by an EOS. If no
// printable glyph survives, `dst` gets a default so no peer renders blank. Exposed
// for the regression test in test/peepo_name.c.
void SanitizePeepoName(u8 *dst, const u8 *src, u32 srcLen);

// Multiplayer overworld presence built on the PeepoNet mailbox transport.
// Broadcasts the local player's map + position + facing, and renders remote
// players as overworld object events when they are on the same map. Called once
// per frame from the main loop.
void PeepoOverworld_Update(void);

// If `localId` is a remote player's avatar, buffers their name into gStringVar1
// and returns the greeting field script; otherwise NULL. Called from the engine's
// object-interaction resolver so pressing A on a remote greets them in-game.
const u8 *PeepoOverworld_GetRemoteInteractScript(u8 localId);

#endif // GUARD_PEEPO_OVERWORLD_H
