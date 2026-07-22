#include "global.h"
#include "peepo_net.h"
#include "peepo_overworld.h"
#include "peepo_mapedit.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "overworld.h"
#include "fieldmap.h"
#include "field_screen_effect.h" // DoTeleportTileWarp (admin teleport-to-player)
#include "wild_encounter.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/species.h" // NUM_SPECIES / SPECIES_NONE (validate network follower species)
#include "constants/characters.h" // CHAR_DYNAMIC / EOS (sanitize network peer names)
#include "main.h"
#include "save.h"
#include "task.h"
#include "sprite.h"
#include "palette.h"
#include "util.h"
#include "script.h"
#include "string_util.h"
#include "constants/rgb.h"

// peepo: a solo, NON-FREEZING battery autosave. Reuses the game's own incremental
// full-save primitives (LinkFullSave_*) but writes ONE sector per frame and skips
// Task_LinkFullSave's GBA-link standby (we have no GBA link — it would hang). With the
// emulator's flash timing now instant, each sector is tiny, so the save spreads smoothly
// over ~15 frames instead of stalling one whole frame (the old ~2s freeze was the flash
// stall; the residual is the save routine's per-byte CPU work, which this spreads out).
// The dual-slot save format makes an interrupted write safe (the loader falls back to the
// previous complete slot).
static void Task_PeepoBatterySave(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (data[0])
    {
    case 0:
        SaveMapView();
        LinkFullSave_Init();
        data[0] = 1;
        break;
    case 1:
        if (LinkFullSave_WriteSector()) // TRUE once all but the last sector are written
            data[0] = 2;                // else: write one more sector next frame
        break;
    case 2:
        LinkFullSave_ReplaceLastSector();
        data[0] = 3;
        break;
    case 3:
        LinkFullSave_SetLastSectorSignature();
        DestroyTask(taskId);
        break;
    }
}

// Kick off the frame-spread battery save (no-op if one is already running).
static void PeepoBatterySave_Start(void)
{
    if (FuncIsActiveTask(Task_PeepoBatterySave))
        return;
    CreateTask(Task_PeepoBatterySave, 5);
}

// Field script (data/event_scripts.s) that shows the greeting message box using
// the name buffered into gStringVar1.
extern const u8 PeepoInteractScript[];
extern const u8 PeepoFollowerScript[];

// Packet protocol (little-endian). All packets are broadcast via the NetRoom
// Durable Object to every other member of the room.
//   HELLO (server->client): [0x00, slot]        assign this client's network id
//   POS   (peer broadcast):  [0x01, id, mapGroup, mapNum, xLo, xHi, yLo, yHi, dir]
#define PKT_HELLO    0x00
#define PKT_POS      0x01  // [0x01, id, mapGroup, mapNum, xLo, xHi, yLo, yHi, dir, gender, fSpecLo, fSpecHi, fFlags, accLo, accHi]
#define PKT_SETNAME  0x03  // JS->ROM (local inject): [0x03, id, gameChars.., 0xFF]
#define PKT_LEAVE    0x04  // JS->ROM (local inject): [0x04, id] — peer left the room
#define PKT_SETCOLOR 0x07  // JS->ROM (local inject): [0x07, colorLo, colorHi] — set OUR accent
                           // (RGB555). Applied locally + shared to peers via the POS tail.
#define PKT_SAVE     0x05  // JS->ROM (local inject): [0x05] — host-initiated timed
                           // battery save; commits SRAM so the cloud autosave (which
                           // syncs the battery save) captures live progress, not just
                           // the player's last manual in-game save.
#define PKT_ADMIN    0x06  // JS->ROM (local inject): [0x06, sub] — room-owner admin command.
                           // Server-gated: the NetRoom DO only relays these from the verified
                           // admin's socket, so a normal client can't forge one.
#define ADMIN_FORCE_ENCOUNTER 0  // sub 0: force a random wild battle on this client right now.
#define ADMIN_FORCE_SPECIES   1  // sub 1: [.., speciesLo, speciesHi, level] force a specific mon.
#define ADMIN_TP_TO           2  // sub 2: [.., slot] warp US to that player's last-known tile.
#define POS_CORE_LEN 10    // fields through gender (accepted even without the follower tail)
#define POS_LEN      15    // + follower species (2) + flags (1) + accent color (2, RGB555|set)

#define MAX_REMOTES 4
#define NAME_LEN    PEEPO_NAME_LEN // remote display-name buffer (see peepo_overworld.h)
#define REMOTE_LOCALID_BASE   0xF0      // object-event localIds for remote avatars (0xF0..0xF3)
#define FOLLOWER_LOCALID_BASE 0xF4      // ... and their follower Pokémon (0xF4..0xF7)
// Frame-based safety net only. Real leaves despawn promptly via PKT_LEAVE (a
// real-time roster event), so this can be generous — it must comfortably exceed
// the JS ~1s keepalive even at 4x fast-forward (where frames elapse 4x faster).
#define REMOTE_TIMEOUT_FRAMES 900       // ~15s @1x, ~3.75s @4x
#define HEARTBEAT_FRAMES 60             // resend our position ~1/s even when idle

#define MQ_SIZE 16                      // per-remote movement queue (power of 2)

struct RemotePlayer
{
    bool8 used;
    bool8 ghost;        // avatar/follower object events couldn't be removed at leave/
                        // timeout (we were in battle/menu); force-remove before the
                        // next reconcile — survives slot reuse by a NEW peer, which
                        // is exactly the case where stale graphics would be inherited
    u8 id;              // network id (peer slot)
    u8 mapGroup;
    u8 mapNum;
    u8 gender;          // MALE / FEMALE — picks the base sprite
    s16 x;              // latest reported tile (target); also the tail of the queue
    s16 y;
    u8 dir;
    u16 accent;         // chosen accent (RGB555 | 0x8000); 0 = unchosen -> per-slot default
    s16 shownX;         // where the sprite currently is (-1 = not placed)
    s16 shownY;
    s16 exitX;          // last tile on our map before a map change (a doorway on warp)
    s16 exitY;
    u8 exitTimer;       // frames to hold the avatar at that doorway before despawning
    u16 lastSeen;       // frame of last POS
    u8 mq[MQ_SIZE];     // queued step directions (replays the peer's real path)
    u8 mqHead;
    u8 mqTail;
    // Networked follower Pokémon (the peer's HGSS-style follower).
    u16 followSpecies;  // 0 = peer has no follower shown
    u8 followFlags;     // bit0 = shiny, bit1 = female
    s16 fShownX;        // follower's shown tile (-1 = not placed)
    s16 fShownY;
    u8 fmq[MQ_SIZE];    // follower path queue (mirrors the player's steps, one tile behind)
    u8 fmqHead;
    u8 fmqTail;
};

// Remote sprite matches the peer's gender; players are told apart by the accent
// color (below) + the roster panel, not by different base sprites.
static u16 RemoteGfx(u8 gender)
{
    return gender == FEMALE ? OBJ_EVENT_GFX_MAY_NORMAL : OBJ_EVENT_GFX_BRENDAN_NORMAL;
}

// Per-slot accent color. Rather than wash the whole palette (which recolors the
// player / shares badly), we give each remote its OWN palette slot = a copy of
// their gender palette with just the green highlight (indices 10/11) recolored
// to the slot color. Values are 0..31 per channel (fed to RGB() at runtime).
#define HL_MAIN    10   // sprite palette index of the bright green highlight
#define HL_SHADOW  11   // ... and its shadow
#define PEEPO_PAL_TAG_BASE 0x7A70  // unique sprite-palette tags 0x7A70..0x7A73
// Avoid green — the sprite's default highlight is green, so a green accent would
// be invisible. Red / blue / yellow / magenta, all clearly distinct from it.
static const u8 sSlotHi[MAX_REMOTES][3] = {
    {28, 11, 11}, // P1 red
    {10, 19, 28}, // P2 blue
    {28, 25, 10}, // P3 yellow
    {25, 11, 28}, // P4 magenta
};
static const u8 sSlotLo[MAX_REMOTES][3] = {
    {18,  6,  6}, // P1 red    (shadow)
    { 6, 12, 18}, // P2 blue
    {18, 16,  6}, // P3 yellow
    {16,  6, 18}, // P4 magenta
};

// Shown when we interact with a peer before their name has arrived from JS.
static const u8 sText_Player[] = _("A traveler");

static EWRAM_DATA struct RemotePlayer sRemotes[MAX_REMOTES] = {0};
// Peer display names in the game charset, indexed by network id (== room slot).
// Pushed from JS (PKT_SETNAME) on roster changes; zero-init means "unset"
// (byte 0 == 0x00, which is the space char — no real name starts with a space).
static EWRAM_DATA u8 sRemoteName[MAX_REMOTES][NAME_LEN] = {0};

// Fallback for a name that filters down to nothing (an all-control-code name), so no
// peer ever renders as a blank nameplate.
static const u8 sDefaultPeepoName[] = _("Player");

// Filter a peer-supplied name to printable glyphs before it is stored and later fed
// to StringExpandPlaceholders (PeepoInteractText's {STR_VAR_1}). Bytes >= CHAR_DYNAMIC
// (0xF7) are text-engine control codes, not glyphs: PLACEHOLDER_BEGIN (0xFD) recurses
// the expander into a remote hard-lock and overruns EWRAM, and the EXT_CTRL / prompt
// codes stall or over-read from a nameplate. Stops at an EOS in `src` (a peer can't
// smuggle bytes past an early terminator) and truncates to the buffer.
void SanitizePeepoName(u8 *dst, const u8 *src, u32 srcLen)
{
    u32 k = 0, i = 0;
    while (i < srcLen && k < NAME_LEN - 1 && src[i] != EOS)
    {
        u8 c = src[i++];
        if (c < CHAR_DYNAMIC)
            dst[k++] = c;
    }
    if (k == 0)
    {
        StringCopy(dst, sDefaultPeepoName);
        return;
    }
    dst[k] = EOS;
}

static EWRAM_DATA bool8 sHasLocalId = FALSE;
static EWRAM_DATA u8 sLocalId = 0;
static EWRAM_DATA u16 sLocalAccent = 0; // our chosen accent (RGB555 | 0x8000); 0 = unchosen
static EWRAM_DATA u16 sFrame = 0;
// EWRAM_DATA must be zero-initialized; 0 is fine as "unset" since real player
// coords are always >= MAP_OFFSET (7), so the first broadcast always fires.
static EWRAM_DATA s16 sLastBcX = 0;
static EWRAM_DATA s16 sLastBcY = 0;
static EWRAM_DATA u8 sLastBcDir = 0;

// True when the player's object event is live on the field (safe to touch the
// object-event system). The callback2 check is essential: gPlayerAvatar /
// gObjectEvents can hold stale-but-"active" data during the new-game intro and
// other non-field screens, and without it our recolor bled onto Prof. Birch.
// Also true in menus, so our POSITION BROADCAST additionally gates on
// FieldRunning() (peer animation no longer does — peers stay live in menus).
static bool8 InOverworld(void)
{
    return gMain.callback2 == CB2_Overworld
        && gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT
        && gObjectEvents[gPlayerAvatar.objectEventId].active
        && !gMain.inBattle;
}

// True only when the LOCAL player is freely walking — false during menus,
// dialogs, scripts and cutscenes. Used to tell whether our own avatar could have
// stepped this frame (gates our position broadcast). Peers animate independently
// of this now (they stay live while we're in a menu), so it no longer freezes them.
static bool8 FieldRunning(void)
{
    return InOverworld() && !ArePlayerFieldControlsLocked();
}

static struct RemotePlayer *FindRemote(u8 id)
{
    u32 i;
    for (i = 0; i < MAX_REMOTES; i++)
        if (sRemotes[i].used && sRemotes[i].id == id)
            return &sRemotes[i];
    return NULL;
}

static struct RemotePlayer *AllocRemote(u8 id)
{
    u32 i;
    for (i = 0; i < MAX_REMOTES; i++)
    {
        if (!sRemotes[i].used)
        {
            struct RemotePlayer *r = &sRemotes[i];
            r->used = TRUE;
            r->id = id;
            r->shownX = -1;
            r->shownY = -1;
            r->mqHead = 0;
            r->mqTail = 0;
            r->followSpecies = 0;
            r->followFlags = 0;
            r->fShownX = -1;
            r->fShownY = -1;
            r->fmqHead = 0;
            r->fmqTail = 0;
            return r;
        }
    }
    return NULL;
}

static void MqClear(struct RemotePlayer *r) { r->mqHead = r->mqTail = 0; }
static bool8 MqEmpty(struct RemotePlayer *r) { return r->mqHead == r->mqTail; }
static u8 MqCount(struct RemotePlayer *r) { return (u8)((r->mqTail - r->mqHead) & (MQ_SIZE - 1)); }
static void MqPush(struct RemotePlayer *r, u8 dir)
{
    u8 next = (u8)((r->mqTail + 1) & (MQ_SIZE - 1));
    if (next == r->mqHead)
        return; // full — the reconcile will snap us forward
    r->mq[r->mqTail] = dir;
    r->mqTail = next;
}
static u8 MqPop(struct RemotePlayer *r)
{
    u8 d = r->mq[r->mqHead];
    r->mqHead = (u8)((r->mqHead + 1) & (MQ_SIZE - 1));
    return d;
}

// Follower path queue (same ring buffer, fed the player's steps one tile behind).
static void FmqClear(struct RemotePlayer *r) { r->fmqHead = r->fmqTail = 0; }
static bool8 FmqEmpty(struct RemotePlayer *r) { return r->fmqHead == r->fmqTail; }
static void FmqPush(struct RemotePlayer *r, u8 dir)
{
    u8 next = (u8)((r->fmqTail + 1) & (MQ_SIZE - 1));
    if (next == r->fmqHead)
        return;
    r->fmq[r->fmqTail] = dir;
    r->fmqTail = next;
}
static u8 FmqPop(struct RemotePlayer *r)
{
    u8 d = r->fmq[r->fmqHead];
    r->fmqHead = (u8)((r->fmqHead + 1) & (MQ_SIZE - 1));
    return d;
}
static u8 FmqCount(struct RemotePlayer *r) { return (u8)((r->fmqTail - r->fmqHead) & (MQ_SIZE - 1)); }

// Pick a walk speed by how many steps we're still behind. A peer moving faster than
// our local tick rate (e.g. they're fast-forwarding, or we're at 1x) piles up queued
// steps; walking Fast/Faster when the backlog is deep lets the avatar catch up to the
// peer's real pace instead of lagging tiles behind (the >6 case snaps outright).
static u8 WalkActionForDepth(u8 dir, u32 depth)
{
    if (depth >= 4) return GetWalkFasterMovementAction(dir);
    if (depth >= 2) return GetWalkFastMovementAction(dir);
    return GetWalkNormalMovementAction(dir);
}

static void BroadcastPos(void)
{
    s16 x, y;
    u8 pkt[POS_LEN];
    u16 fSpecies = 0;
    u8 fFlags = 0;
    PlayerGetDestCoords(&x, &y);
    pkt[0] = PKT_POS;
    pkt[1] = sLocalId;
    pkt[2] = gSaveBlock1Ptr->location.mapGroup;
    pkt[3] = gSaveBlock1Ptr->location.mapNum;
    pkt[4] = x & 0xFF;
    pkt[5] = (x >> 8) & 0xFF;
    pkt[6] = y & 0xFF;
    pkt[7] = (y >> 8) & 0xFF;
    pkt[8] = GetPlayerFacingDirection();
    pkt[9] = gSaveBlock2Ptr->playerGender;
    // Follower Pokémon: only advertise it while it's actually shown on the field.
    if (GetFollowerObject() != NULL)
    {
        u32 sp;
        bool32 shiny = FALSE, female = FALSE;
        if (GetFollowerInfo(&sp, &shiny, &female))
        {
            fSpecies = sp;
            fFlags = (shiny ? 1 : 0) | (female ? 2 : 0);
        }
    }
    pkt[10] = fSpecies & 0xFF;
    pkt[11] = (fSpecies >> 8) & 0xFF;
    pkt[12] = fFlags;
    pkt[13] = sLocalAccent & 0xFF;        // our accent (RGB555 | 0x8000; 0 = unchosen)
    pkt[14] = (sLocalAccent >> 8) & 0xFF;
    PeepoNet_Send(pkt, POS_LEN);
}

static void HandlePos(const u8 *p, u32 n)
{
    struct RemotePlayer *r;
    u8 id, nmg, nmn, ndir;
    s16 nx, ny, dx, dy, total;
    bool8 isNew = FALSE;
    if (n < POS_CORE_LEN)
        return;
    id = p[1];
    if (sHasLocalId && id == sLocalId)
        return; // ignore our own echo
    r = FindRemote(id);
    if (r == NULL)
    {
        r = AllocRemote(id);
        isNew = TRUE;
    }
    if (r == NULL)
        return;

    nmg = p[2];
    nmn = p[3];
    nx = (s16)(p[4] | (p[5] << 8));
    ny = (s16)(p[6] | (p[7] << 8));
    ndir = p[8];
    r->dir = ndir;
    r->gender = p[9];
    if (n >= POS_CORE_LEN + 3) // follower tail: species (2) + flags (1)
    {
        r->followSpecies = (u16)(p[10] | (p[11] << 8));
        if (r->followSpecies >= NUM_SPECIES) // an out-of-range species reads gSpeciesInfo[] before the graphics guard; 0 already means "no follower shown"
            r->followSpecies = SPECIES_NONE;
        r->followFlags = p[12];
    }
    if (n >= POS_LEN) // accent tail: color (2), RGB555 | 0x8000 (0 = unchosen)
        r->accent = (u16)(p[13] | (p[14] << 8));
    r->lastSeen = sFrame;

    if (isNew || nmg != r->mapGroup || nmn != r->mapNum)
    {
        // New peer or a map change: jump the target and clear the path queues so
        // the reconcile places/snaps the avatar (and follower) fresh. Stash the last
        // tile they held on the OLD map (a doorway when they warp out) so, if that
        // old map is ours, the reconcile can snap the laggy avatar to the door before
        // despawning it instead of blinking out wherever the replay had lagged to.
        r->exitX = isNew ? -1 : r->x;
        r->exitY = isNew ? -1 : r->y;
        r->exitTimer = isNew ? 0 : 12;
        r->mapGroup = nmg;
        r->mapNum = nmn;
        r->x = nx;
        r->y = ny;
        MqClear(r);
        FmqClear(r);
        r->shownX = -1;
        r->fShownX = -1;
        return;
    }

    // Same map: enqueue the individual tile steps from the last target to the
    // new one, so the avatar replays the peer's ACTUAL path (never cutting
    // through walls). Straight runs of >1 tile enqueue multiple steps.
    dx = nx - r->x;
    dy = ny - r->y;
    total = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (total > MQ_SIZE - 2)
    {
        // Way behind (dropped packets / teleport): snap instead of a long walk.
        MqClear(r);
    }
    else
    {
        while (dx > 0) { MqPush(r, DIR_EAST);  dx--; }
        while (dx < 0) { MqPush(r, DIR_WEST);  dx++; }
        while (dy > 0) { MqPush(r, DIR_SOUTH); dy--; }
        while (dy < 0) { MqPush(r, DIR_NORTH); dy++; }
    }
    r->x = nx;
    r->y = ny;
}

// Resolve a player's accent to its highlight (hi) + shadow (lo) colors. accent bit15 set =
// an explicitly CHOSEN color (low 15 bits, RGB555); the shadow is derived ~0.6x. accent 0 =
// unchosen -> the legacy per-slot color, so anyone who never picks still has a distinct one.
static void AccentColors(u16 accent, u32 slot, u16 *hi, u16 *lo)
{
    if (accent & 0x8000)
    {
        u32 r = accent & 0x1F, g = (accent >> 5) & 0x1F, b = (accent >> 10) & 0x1F;
        *hi = (u16)(accent & 0x7FFF);
        *lo = RGB((r * 3) / 5, (g * 3) / 5, (b * 3) / 5);
    }
    else
    {
        u32 s = slot & (MAX_REMOTES - 1);
        *hi = RGB(sSlotHi[s][0], sSlotHi[s][1], sSlotHi[s][2]);
        *lo = RGB(sSlotLo[s][0], sSlotLo[s][1], sSlotLo[s][2]);
    }
}

// Give this remote its own OBJ palette slot (keyed by its network slot's tag) and keep its
// sprite pointed at it. The palette = the base gender palette with the highlight/shadow
// recolored to the player's accent (their CHOSEN color, or their per-slot default if unset).
// Its own slot, so it never touches the player's or another peer's colors. Refreshed every
// reconcile frame, so a live recolor takes effect at once. The tag is NOT blend-immune, so
// UpdateSpritePaletteWithTime day/night-tints it just like the other overworld sprites.
static void ApplyRemotePalette(u8 oeId, struct RemotePlayer *r)
{
    struct Sprite *sprite = &gSprites[gObjectEvents[oeId].spriteId];
    u32 s = r->id & (MAX_REMOTES - 1);
    u16 tag = PEEPO_PAL_TAG_BASE + s;
    u8 slot = IndexOfSpritePaletteTag(tag);
    u16 hi, lo, buf[16];
    u8 srcPal = sprite->oam.paletteNum; // gender palette on first load; the accent slot after
    u32 k;

    // srcPal already holds the gender colors at every index except HL_*, so reading it back and
    // re-writing HL_* keeps the gender base AND picks up a live color change.
    for (k = 0; k < 16; k++)
        buf[k] = gPlttBufferUnfaded[OBJ_PLTT_ID(srcPal) + k];
    AccentColors(r->accent, s, &hi, &lo);
    buf[HL_MAIN]   = hi;
    buf[HL_SHADOW] = lo;

    if (slot == 0xFF)
    {
        struct SpritePalette sp;
        sp.data = buf;
        sp.tag = tag;
        slot = LoadSpritePalette(&sp);
    }
    else
    {
        for (k = 0; k < 16; k++) // already loaded: refresh contents so a recolor takes effect
            gPlttBufferUnfaded[OBJ_PLTT_ID(slot) + k] = buf[k];
    }
    if (slot != 0xFF)
    {
        sprite->oam.paletteNum = slot;
        if (!gPaletteFade.active)
            UpdateSpritePaletteWithTime(slot); // day/night tint into the faded buffer
    }
}

static void FreeRemotePalette(u32 colorSlot)
{
    FreeSpritePaletteByTag(PEEPO_PAL_TAG_BASE + (colorSlot & (MAX_REMOTES - 1)));
}

// Recolor the LOCAL player's own character to their room-slot accent, so you see
// yourself in the same color everyone else sees you (and matching the roster).
// Re-applied every frame to survive palette reloads (bike/surf/day-night).
// We write the accent into the UNFADED palette and let the game's pipeline build
// FADED: outdoors UpdateSpritePaletteWithTime applies the day/night tint, and
// during a palette fade — or any non-field state (menus, scripts, warps) — we leave
// FADED to the game so the accent dims/holds with everything else instead of flashing.
static void RecolorLocalPlayer(void)
{
    struct Sprite *sprite;
    u32 s = sLocalId & (MAX_REMOTES - 1);
    u16 off, hi, lo;
    if (!sHasLocalId || gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return;
    sprite = &gSprites[gObjectEvents[gPlayerAvatar.objectEventId].spriteId];
    off = OBJ_PLTT_ID(sprite->oam.paletteNum);
    AccentColors(sLocalAccent, s, &hi, &lo);
    gPlttBufferUnfaded[off + HL_MAIN]   = hi;
    gPlttBufferUnfaded[off + HL_SHADOW] = lo;
    // Only touch the on-screen (FADED) buffer while FREELY on the field. In menus, scripts
    // and warp transitions the game dims/holds the palette itself; force-writing FADED there
    // snapped the accent back to full brightness for a moment (the "color flash" on opening a
    // menu / entering a house). Skipping it lets the accent dim with everything; UNFADED above
    // stays the target, so it re-appears the instant control returns.
    if (!gPaletteFade.active && FieldRunning())
    {
        gPlttBufferFaded[off + HL_MAIN]   = hi; // correct indoors (no tint)
        gPlttBufferFaded[off + HL_SHADOW] = lo;
        UpdateSpritePaletteWithTime(sprite->oam.paletteNum); // re-tint outdoors
    }
}

// Where a remote's avatar should render, in OUR map's runtime coords. Same map →
// their own coords (smooth path-replay). On a directly-CONNECTED adjacent map (the
// tiles stitched onto a screen edge) → translate their coords into our frame via the
// connection offset so they stay visible walking across the seam. Off our map and not
// a nearby connection → FALSE (despawn). `*sameMap` distinguishes the first two. The
// transform mirrors the engine's SetPositionFromConnection.
static bool8 RemoteRenderPos(struct RemotePlayer *r, s16 *outX, s16 *outY, bool8 *sameMap)
{
    u8 myG = gSaveBlock1Ptr->location.mapGroup, myN = gSaveBlock1Ptr->location.mapNum;
    const struct MapConnection *c;
    s32 count, i;

    if (r->x < MAP_OFFSET || r->y < MAP_OFFSET)
        return FALSE; // no valid position yet
    if (r->mapGroup == myG && r->mapNum == myN)
    {
        *outX = r->x;
        *outY = r->y;
        *sameMap = TRUE;
        return TRUE;
    }
    *sameMap = FALSE;
    if (gMapHeader.mapLayout == NULL || gMapHeader.connections == NULL)
        return FALSE;

    count = gMapHeader.connections->count;
    c = gMapHeader.connections->connections;
    for (i = 0; i < count; i++, c++)
    {
        const struct MapHeader *mh;
        s16 myw, myh, cw, ch, mrx, mry, lx, ly;
        if (c->mapGroup != r->mapGroup || c->mapNum != r->mapNum)
            continue;
        mh = GetMapHeaderFromConnection(c);
        if (mh == NULL || mh->mapLayout == NULL)
            return FALSE;
        myw = gMapHeader.mapLayout->width;
        myh = gMapHeader.mapLayout->height;
        cw = mh->mapLayout->width;
        ch = mh->mapLayout->height;
        mrx = r->x - MAP_OFFSET; // their map-local tile
        mry = r->y - MAP_OFFSET;
        switch (c->direction)
        {
        case CONNECTION_NORTH: lx = mrx + c->offset; ly = mry - ch;  break;
        case CONNECTION_SOUTH: lx = mrx + c->offset; ly = mry + myh; break;
        case CONNECTION_WEST:  lx = mrx - cw;        ly = mry + c->offset; break;
        case CONNECTION_EAST:  lx = mrx + myw;       ly = mry + c->offset; break;
        default: return FALSE; // dive/emerge — not a screen-adjacent connection
        }
        // Only render if it lands in the rendered border region (else it's off-screen
        // and the runtime coord would fall outside the map grid).
        if (lx < -MAP_OFFSET || lx >= myw + MAP_OFFSET || ly < -MAP_OFFSET || ly >= myh + MAP_OFFSET)
            return FALSE;
        *outX = (s16)(lx + MAP_OFFSET);
        *outY = (s16)(ly + MAP_OFFSET);
        return TRUE;
    }
    return FALSE;
}

// Reconcile one remote's avatar with the field. Spawns/despawns by map; walks
// the queued path one tile per finished step; snaps on big drift. Peers keep
// animating even while we're in a menu/dialog (their sprites are re-thawed each
// frame), so there's no backlog to snap through on exit.
static void RemoveRemoteFollower(u32 slotIdx); // defined below; needed by the ghost sweep here

static void ReconcileRemote(struct RemotePlayer *r, u32 slotIdx)
{
    u8 localId = REMOTE_LOCALID_BASE + slotIdx;
    u8 myMapGroup = gSaveBlock1Ptr->location.mapGroup;
    u8 myMapNum = gSaveBlock1Ptr->location.mapNum;

    // Slot reused by a NEW peer before the orphan sweep could run (packets drain
    // during battle): the previous occupant's avatar/follower are still on the
    // field and would be silently adopted with stale gender/shiny graphics.
    // Force-remove them first so this peer spawns fresh. (Only reached when
    // InOverworld() — the caller gates on it.)
    if (r->ghost)
    {
        RemoveObjectEventByLocalIdAndMap(localId, myMapNum, myMapGroup);
        RemoveRemoteFollower(slotIdx);
        r->ghost = FALSE;
    }
    s16 efx = 0, efy = 0;      // effective render position in OUR coord frame
    bool8 sameMap = FALSE;
    bool8 render = RemoteRenderPos(r, &efx, &efy, &sameMap);
    u8 oeId = GetObjectEventIdByLocalIdAndMap(localId, myMapNum, myMapGroup);
    bool8 spawned = oeId != OBJECT_EVENTS_COUNT;
    struct ObjectEvent *oe;

    if (!render)
    {
        if (spawned)
        {
            struct ObjectEvent *lo = &gObjectEvents[oeId];
            // They left our map (e.g. warped into a house). The path-replay may have
            // lagged the avatar tiles behind, so snap it to the doorway (their last
            // tile here) and hold a few frames — it visibly reaches the door instead
            // of blinking out mid-map — then despawn.
            if (r->exitTimer > 0 && r->exitX >= MAP_OFFSET && r->exitY >= MAP_OFFSET)
            {
                if (r->shownX != r->exitX || r->shownY != r->exitY)
                {
                    MoveObjectEventToMapCoords(lo, r->exitX, r->exitY);
                    r->shownX = r->exitX;
                    r->shownY = r->exitY;
                    ObjectEventSetHeldMovement(lo, GetFaceDirectionMovementAction(r->dir));
                }
                r->exitTimer--;
                return; // hold at the doorway this tick
            }
            RemoveObjectEventByLocalIdAndMap(localId, myMapNum, myMapGroup);
            FreeRemotePalette(r->id);
            r->shownX = -1;
            r->shownY = -1;
            r->exitX = -1;
            r->exitTimer = 0;
        }
        return;
    }

    // We can render them here (our map or a visible connection), so this isn't a
    // warp-out — cancel any pending doorway-snap. If they later walk off-screen (deep
    // into a neighbour) they'll then just despawn in place, not snap back to the seam.
    r->exitTimer = 0;

    if (!spawned)
    {
        // Spawn where the sprite currently is (same-map re-appear) else at the target,
        // so it doesn't jump. Connected-map avatars always spawn at the translated pos.
        s16 sx = (sameMap && r->shownX >= MAP_OFFSET) ? r->shownX : efx;
        s16 sy = (sameMap && r->shownY >= MAP_OFFSET) ? r->shownY : efy;
        SpawnSpecialObjectEventParameterized(RemoteGfx(r->gender),
                                             MOVEMENT_TYPE_NONE, localId, sx, sy, 3);
        oeId = GetObjectEventIdByLocalIdAndMap(localId, myMapNum, myMapGroup);
        if (oeId == OBJECT_EVENTS_COUNT)
            return;
        r->shownX = sx;
        r->shownY = sy;
        ApplyRemotePalette(oeId, r);
        return;
    }

    oe = &gObjectEvents[oeId];

    // Keep the sprite pointed at its own recolored palette (guards against the
    // object-event system reassigning it back to the shared gender palette).
    ApplyRemotePalette(oeId, r);

    // Peers move in real time even while WE are in a menu / dialog / cutscene —
    // your local UI must not pause other players. Those states call
    // FreezeObjectEvents(), which pauses remote sprites too, so re-thaw ours
    // every frame: a frozen object event's held movement never advances, so the
    // peer would stall and then snap to catch up on exit (the teleport).
    if (oe->frozen)
        UnfreezeObjectEvent(oe);

    if (!ObjectEventClearHeldMovementIfFinished(oe))
        return; // still walking the previous step

    // The path queue holds step DIRECTIONS, which read identically in a connected
    // neighbour's frame and ours (connections are axis-aligned — no rotation), and the
    // coordinate translation is a pure offset, so replaying the same directions lands
    // the avatar exactly at the translated tile. So we smooth-walk the peer's path in
    // BOTH cases; only the TARGET differs — `efx/efy` is their tile in OUR frame
    // (== r->x/r->y on our own map, the translated tile on a connected one). At most
    // one small snap happens as they cross the seam (the queue is reset on map change).

    // If we've fallen far behind (long queue), snap the surplus so we don't lag.
    if (MqCount(r) > 6)
    {
        MoveObjectEventToMapCoords(oe, efx, efy);
        r->shownX = efx;
        r->shownY = efy;
        MqClear(r);
        ObjectEventSetHeldMovement(oe, GetFaceDirectionMovementAction(r->dir));
        return;
    }

    if (!MqEmpty(r))
    {
        u8 dir = MqPop(r);
        switch (dir)
        {
        case DIR_EAST:  r->shownX++; break;
        case DIR_WEST:  r->shownX--; break;
        case DIR_SOUTH: r->shownY++; break;
        case DIR_NORTH: r->shownY--; break;
        }
        FmqPush(r, dir); // the follower replays the same step, one tile behind
        ObjectEventSetHeldMovement(oe, WalkActionForDepth(dir, MqCount(r)));
    }
    else
    {
        // Idle: correct any residual drift, then face the reported direction.
        if (r->shownX != efx || r->shownY != efy)
        {
            MoveObjectEventToMapCoords(oe, efx, efy);
            r->shownX = efx;
            r->shownY = efy;
        }
        ObjectEventSetHeldMovement(oe, GetFaceDirectionMovementAction(r->dir));
    }
}

static void RemoveRemoteFollower(u32 slotIdx)
{
    u8 flid = FOLLOWER_LOCALID_BASE + slotIdx;
    u8 myMapNum = gSaveBlock1Ptr->location.mapNum;
    u8 myMapGroup = gSaveBlock1Ptr->location.mapGroup;
    if (GetObjectEventIdByLocalIdAndMap(flid, myMapNum, myMapGroup) != OBJECT_EVENTS_COUNT)
        RemoveObjectEventByLocalIdAndMap(flid, myMapNum, myMapGroup);
    sRemotes[slotIdx].fShownX = -1;
    sRemotes[slotIdx].fShownY = -1;
    FmqClear(&sRemotes[slotIdx]);
}

// Cardinal direction pointing from (fx,fy) toward (tx,ty) — used so the follower
// looks AT the player's tile rather than mirroring the player's facing (which only
// looks right while trailing, not when the player turns in place).
static u8 DirToward(s16 fx, s16 fy, s16 tx, s16 ty)
{
    s16 dx = tx - fx, dy = ty - fy;
    s16 adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (ady >= adx)
        return dy >= 0 ? DIR_SOUTH : DIR_NORTH;
    return dx >= 0 ? DIR_EAST : DIR_WEST;
}

// The tile one step behind a player facing `dir` — where the follower trails.
static void TileBehind(s16 x, s16 y, u8 dir, s16 *bx, s16 *by)
{
    *bx = x;
    *by = y;
    switch (dir)
    {
    case DIR_SOUTH: (*by)--; break; // facing down -> behind is up
    case DIR_NORTH: (*by)++; break;
    case DIR_EAST:  (*bx)--; break;
    case DIR_WEST:  (*bx)++; break;
    default:        (*by)--; break;
    }
}

// Spawn/despawn/move a peer's follower Pokémon so it trails their avatar one tile
// behind, replaying the same path (fed via FmqPush in ReconcileRemote).
static void ReconcileRemoteFollower(struct RemotePlayer *r, u32 slotIdx)
{
    u8 flid = FOLLOWER_LOCALID_BASE + slotIdx;
    u8 plid = REMOTE_LOCALID_BASE + slotIdx;
    u8 myMapNum = gSaveBlock1Ptr->location.mapNum;
    u8 myMapGroup = gSaveBlock1Ptr->location.mapGroup;
    u8 fOeId = GetObjectEventIdByLocalIdAndMap(flid, myMapNum, myMapGroup);
    bool8 fSpawned = fOeId != OBJECT_EVENTS_COUNT;
    u8 pOeId = GetObjectEventIdByLocalIdAndMap(plid, myMapNum, myMapGroup);
    struct ObjectEvent *foe;
    s16 bx, by, dist;

    // No follower, or the peer's avatar isn't spawned here -> ensure it's gone.
    if (r->followSpecies == 0 || pOeId == OBJECT_EVENTS_COUNT)
    {
        if (fSpawned)
            RemoveRemoteFollower(slotIdx);
        return;
    }

    if (!fSpawned)
    {
        u16 gfx = (u16)(r->followSpecies + OBJ_EVENT_MON
                        + ((r->followFlags & 1) ? OBJ_EVENT_MON_SHINY : 0)
                        + ((r->followFlags & 2) ? OBJ_EVENT_MON_FEMALE : 0));
        TileBehind(r->shownX, r->shownY, r->dir, &bx, &by);
        SpawnSpecialObjectEventParameterized(gfx, MOVEMENT_TYPE_NONE, flid, bx, by,
                                             gObjectEvents[pOeId].currentElevation);
        fOeId = GetObjectEventIdByLocalIdAndMap(flid, myMapNum, myMapGroup);
        if (fOeId == OBJECT_EVENTS_COUNT)
            return;
        r->fShownX = bx;
        r->fShownY = by;
        FmqClear(r);
        // Face the peer on first appearance. After this the follower keeps its
        // travel-direction facing (idle never re-faces), matching the local one.
        ObjectEventSetHeldMovement(&gObjectEvents[fOeId],
            GetFaceDirectionMovementAction(DirToward(bx, by, r->shownX, r->shownY)));
        return;
    }

    foe = &gObjectEvents[fOeId];

    // Peer switched their lead Pokémon -> respawn with the new sprite.
    if (OW_SPECIES(foe) != r->followSpecies)
    {
        RemoveRemoteFollower(slotIdx);
        return;
    }

    if (foe->frozen)
        UnfreezeObjectEvent(foe); // keep followers live in menus too (see ReconcileRemote)
    if (!ObjectEventClearHeldMovementIfFinished(foe))
        return; // still walking the previous step

    // Drifted far behind (avatar snapped / dropped packets) -> snap in behind them.
    dist = (r->fShownX > r->shownX ? r->fShownX - r->shownX : r->shownX - r->fShownX)
         + (r->fShownY > r->shownY ? r->fShownY - r->shownY : r->shownY - r->fShownY);
    if (dist > 3)
    {
        TileBehind(r->shownX, r->shownY, r->dir, &bx, &by);
        MoveObjectEventToMapCoords(foe, bx, by);
        r->fShownX = bx;
        r->fShownY = by;
        FmqClear(r);
        ObjectEventSetHeldMovement(foe,
            GetFaceDirectionMovementAction(DirToward(r->fShownX, r->fShownY, r->shownX, r->shownY)));
        return;
    }

    if (!FmqEmpty(r))
    {
        u8 dir = FmqPop(r);
        switch (dir)
        {
        case DIR_EAST:  r->fShownX++; break;
        case DIR_WEST:  r->fShownX--; break;
        case DIR_SOUTH: r->fShownY++; break;
        case DIR_NORTH: r->fShownY--; break;
        }
        ObjectEventSetHeldMovement(foe, WalkActionForDepth(dir, FmqCount(r)));
    }
    // else: idle — keep the follower's current (travel-direction) facing, exactly
    // like the local follower (FollowablePlayerMovement_Idle never turns to face the
    // player). Re-facing via DirToward every frame is what made it jitter/snap and
    // "look off"; the walk step above already faces the direction of travel.
}

// Called from the engine's native interaction path (GetInteractedObjectEventScript
// in field_control_avatar.c) when the player presses A facing an object that has
// no map script. If that object is one of our remote avatars, buffer its name
// into gStringVar1 and hand back the greeting script — so the box pops through
// the normal, safely-locked field flow (no main-loop A-poll racing the engine).
const u8 *PeepoOverworld_GetRemoteInteractScript(u8 localId)
{
    u32 idx;
    struct RemotePlayer *r;
    u8 nameIdx;
    bool8 isFollower;

    if (localId >= REMOTE_LOCALID_BASE && localId < REMOTE_LOCALID_BASE + MAX_REMOTES)
    {
        idx = localId - REMOTE_LOCALID_BASE;
        isFollower = FALSE;
    }
    else if (localId >= FOLLOWER_LOCALID_BASE && localId < FOLLOWER_LOCALID_BASE + MAX_REMOTES)
    {
        // A remote follower is also template-less; resolve it here too so the
        // engine never dereferences its (null) map script.
        idx = localId - FOLLOWER_LOCALID_BASE;
        isFollower = TRUE;
    }
    else
    {
        return NULL;
    }

    r = &sRemotes[idx];
    if (!r->used)
        return NULL;

    nameIdx = r->id & (MAX_REMOTES - 1);
    if (sRemoteName[nameIdx][0] != 0)
        StringCopy(gStringVar1, sRemoteName[nameIdx]);
    else
        StringCopy(gStringVar1, sText_Player);
    return isFollower ? PeepoFollowerScript : PeepoInteractScript;
}

// peepo: admin "teleport to a player" — warp US to the last-known map/tile of the remote
// player with the given network id (= their room slot). That position is what we already
// track to draw their avatar, so no extra round-trip is needed. No-op if the slot isn't
// present (they left / never seen). Reuses the game's teleport-tile warp: fade out, load
// their map, spin us in, and hand back normal field control.
static void PeepoTeleportToRemote(u8 id)
{
    struct RemotePlayer *r = FindRemote(id);

    if (r == NULL)
        return;
    // r->x / r->y are object-event coords (they include MAP_OFFSET); the warp wants
    // map-local coords, so strip the offset. XY16 variant: SetWarpDestination's s8
    // x/y params truncate coords >= 128, miswarping on large maps.
    SetWarpDestinationXY16(r->mapGroup, r->mapNum, r->x - MAP_OFFSET, r->y - MAP_OFFSET);
    DoTeleportTileWarp();
}

void PeepoOverworld_Update(void)
{
    static u8 rx[NET_MAX];
    u32 i, n;

    if (!PeepoNet_Open())
        return;

    sFrame++;

    // Drain inbound packets (single-slot transport delivers one per poll).
    while ((n = PeepoNet_Poll(rx)) != 0)
    {
        if (rx[0] == PKT_HELLO && n >= 2)
        {
            sLocalId = rx[1];
            sHasLocalId = TRUE;
        }
        else if (rx[0] == PKT_POS)
        {
            HandlePos(rx, n);
        }
        else if (rx[0] == PKT_SETNAME && n >= 2)
        {
            // Filter to printable glyphs (see SanitizePeepoName): raw peer bytes reach
            // the text engine via the nameplate, where control codes recurse or stall it.
            SanitizePeepoName(sRemoteName[rx[1] & (MAX_REMOTES - 1)], &rx[2], n - 2);
        }
        else if (rx[0] == PKT_SETCOLOR && n >= 3)
        {
            // JS -> our own ROM: set OUR accent (RGB555 in the low 15 bits). RecolorLocalPlayer
            // applies it to our sprite; BroadcastPos shares it with peers via the POS tail.
            sLocalAccent = (u16)(((rx[1] | (rx[2] << 8)) & 0x7FFF) | 0x8000);
        }
        else if (rx[0] == PKT_LEAVE && n >= 2)
        {
            // Roster says a peer left — despawn promptly (real-time), rather than
            // waiting out the frame timeout (which drifts under fast-forward).
            struct RemotePlayer *r = FindRemote(rx[1]);
            if (r != NULL)
            {
                if (InOverworld())
                {
                    RemoveObjectEventByLocalIdAndMap(REMOTE_LOCALID_BASE + (u32)(r - sRemotes),
                        gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);
                    RemoveRemoteFollower((u32)(r - sRemotes));
                }
                else
                {
                    r->ghost = TRUE; // can't touch object events now — force-remove later
                }
                FreeRemotePalette(r->id);
                r->used = FALSE;
            }
            sRemoteName[rx[1] & (MAX_REMOTES - 1)][0] = 0; // forget the name
        }
        else if (rx[0] == PKT_SAVE)
        {
            // Host-initiated timed autosave: commit the battery save, but ONLY when
            // freely on the field (FieldRunning() is false in battles, menus, scripts
            // and cutscenes) so we never persist a transient mid-sequence state or
            // fight the save the game is already doing.
            if (FieldRunning())
                PeepoBatterySave_Start(); // frame-spread, non-freezing (was TrySavingData)
        }
        else if (rx[0] == PKT_ADMIN && n >= 2 && FieldRunning())
        {
            // Room-owner admin command (server-gated by the NetRoom DO). Only acted on
            // when freely on the field, so it never fires mid-battle/menu/script.
            if (rx[1] == ADMIN_FORCE_ENCOUNTER)
                PeepoForceEncounter(); // random mon from the local area
            else if (rx[1] == ADMIN_FORCE_SPECIES && n >= 5)
                PeepoForceEncounterSpecies(rx[2] | (rx[3] << 8), rx[4]); // [.., sLo, sHi, level]
            else if (rx[1] == ADMIN_TP_TO && n >= 3)
                PeepoTeleportToRemote(rx[2]); // warp us to that player's slot
        }
        else
        {
            // Map plane (MAP_* opcodes): the shared world map editor. Ignores
            // anything it doesn't recognize, so this is safe as the catch-all.
            PeepoMapEdit_OnPacket(rx, n);
        }
    }

    // Broadcast our position when it changes (captures each tile step for the path
    // replay). The periodic heartbeat fires even when the field is FROZEN (in the
    // QOL menu / map editor) so peers don't age us out and despawn our avatar while
    // we're standing still in a menu.
    if (sHasLocalId && InOverworld())
    {
        s16 x, y;
        u8 dir;
        bool8 moved;
        PlayerGetDestCoords(&x, &y);
        dir = GetPlayerFacingDirection();
        moved = FieldRunning() && (x != sLastBcX || y != sLastBcY || dir != sLastBcDir);
        if (moved || (sFrame % HEARTBEAT_FRAMES) == 0)
        {
            BroadcastPos();
            sLastBcX = x;
            sLastBcY = y;
            sLastBcDir = dir;
        }
    }

    // (Interaction is handled by the engine's native object-interaction path via
    // PeepoOverworld_GetRemoteInteractScript() — hooked into
    // GetInteractedObjectEventScript in field_control_avatar.c — so pressing A
    // facing a remote avatar runs the greeting through the normal, safe flow.)

    // Tint your own character to your room-slot color too (matches the roster and
    // how peers see you).
    if (InOverworld())
        RecolorLocalPlayer();

    // Age out peers we've stopped hearing from, then reconcile the rest. Only
    // manipulate object events while safely in the overworld.
    for (i = 0; i < MAX_REMOTES; i++)
    {
        struct RemotePlayer *r = &sRemotes[i];
        if (!r->used)
        {
            // A peer that left or timed out while we were in a battle/menu couldn't
            // be despawned at that moment (object events are only safe to touch in
            // the overworld), and the return-to-field path re-materializes every
            // still-active object event — leaving a frozen, collidable ghost until
            // the next real map warp. Sweep the orphan here instead; both removals
            // are existence-checked no-ops when nothing lingers.
            if (InOverworld())
            {
                RemoveObjectEventByLocalIdAndMap(REMOTE_LOCALID_BASE + i,
                    gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);
                RemoveRemoteFollower(i);
                r->ghost = FALSE; // orphans (if any) are gone
            }
            continue;
        }
        if ((u16)(sFrame - r->lastSeen) > REMOTE_TIMEOUT_FRAMES)
        {
            if (InOverworld())
            {
                RemoveObjectEventByLocalIdAndMap(REMOTE_LOCALID_BASE + i,
                    gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);
                RemoveRemoteFollower(i);
            }
            else
            {
                r->ghost = TRUE; // can't touch object events now — force-remove later
            }
            FreeRemotePalette(r->id);
            r->used = FALSE;
            continue;
        }
        if (InOverworld())
        {
            ReconcileRemote(r, i);
            ReconcileRemoteFollower(r, i);
        }
    }
}
