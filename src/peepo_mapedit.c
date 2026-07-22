#include "global.h"
#include "peepo_net.h"
#include "peepo_mapedit.h"
#include "fieldmap.h"
#include "field_camera.h"
#include "overworld.h" // gPeepoWarpGen (detect same-map warps that rebuild the map)
#include "field_player_avatar.h"
#include "event_object_movement.h"
#include "event_object_lock.h"
#include "metatile_behavior.h"
#include "script.h"
#include "sprite.h"
#include "palette.h"
#include "task.h"
#include "menu.h"
#include "window.h"
#include "text.h"
#include "string_util.h"
#include "sound.h"
#include "main.h"
#include "field_move.h"
#include "pokemon.h"
#include "party_menu.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/metatile_labels.h"
#include "constants/rgb.h"
#include "constants/songs.h"

// Wire protocol (little-endian). Shares the mailbox with the presence netcode;
// map-plane opcodes start at 0x10. Coordinates on the wire are MAP-LOCAL (0-based,
// matching the map's own events), i.e. runtime tile coord minus MAP_OFFSET.
//   MAP_EDIT 0x10  [op, g, n, xLo,xHi, yLo,yHi, valLo,valHi, flags]  set/delete a cell
//   MAP_REQ  0x11  [op, g, n]                                        request a map's edits
//   MAP_SNAP 0x12  [op, g, n, more, count, {xLo,xHi,yLo,yHi,valLo,valHi}*count]
#define PKT_MAP_EDIT     0x10
#define PKT_MAP_REQ      0x11
#define PKT_MAP_SNAP     0x12
#define PKT_MAP_OBJ      0x13  // placed-object edit (same 10-byte shape as MAP_EDIT; val = gfx id)
#define PKT_MAP_OBJ_SNAP 0x14  // placed-object snapshot (same shape as MAP_SNAP)

#define EDIT_DELETE  0x01  // MAP_EDIT/MAP_OBJ flags bit0 — revert the cell / remove the object

#define CURSOR_LOCALID   0xEF  // object-event slot for the editor cursor's object ghost
#define CURSOR_PAL_TAG   0x5EED // OBJ palette tag for the mode-tinted selection box

// Keep the cursor within the visible screen around where editing began (no camera
// scrolling in v1; walk + re-enter to edit farther). The view is ~15 metatiles wide
// (player-centred → ±7) and ~10 tall (±5 reaches the top/bottom edge tiles, e.g. the
// tree walls). The earlier "goes off-screen" was the mispositioned sprite, not range.
#define CURSOR_RANGE_X   7
#define CURSOR_RANGE_Y   5

#define MAPEDIT_WIN_BASEBLOCK 0x139  // shared with the (mutually exclusive) start/QOL menu

// Collision value written for a WALL tile (bit 10 of MAPGRID_COLLISION_MASK).
// Any non-zero collision makes the cell impassable.
#define WALL_COLLISION (1 << 10)

// Networking state — runs every frame regardless of the editor UI, so the shared
// world renders while just walking around.
static EWRAM_DATA bool8 sHaveMap = FALSE;
static EWRAM_DATA u8 sMapGroup = 0;
static EWRAM_DATA u8 sMapNum = 0;
// gPeepoWarpGen as of our last map (re)detection. ApplyCurrentWarp bumps that
// counter on every warp, so it fires as an EVENT even when a warp lands back on the
// same tile of the same map (where a value-diff of gLastUsedWarp would not change),
// signalling a same-map rebuild that group/num alone can't see.
static EWRAM_DATA u32 sLastWarpGen = 0;
// The persisted-edit snapshot IS received on first entry (confirmed via server
// logs), but an early application gets wiped when the map finishes loading/redrawing
// at boot — so the edits only show after you re-enter. Fix: re-request the snapshot
// several times over the first few seconds so a copy lands AFTER the map settles
// (SetCell is idempotent, so re-applying the same cells is harmless).
static EWRAM_DATA u8 sSnapReqLeft = 0; // re-requests still to send after map entry
static EWRAM_DATA u16 sSnapTimer = 0;
#define SNAP_REQ_TOTAL  2   // a couple of retries for redundancy against a dropped reply
#define SNAP_REQ_FRAMES 45

// Editor UI state (only meaningful while the editor task is alive).
static EWRAM_DATA s16 sCursorX = 0;      // runtime tile coords (>= MAP_OFFSET)
static EWRAM_DATA s16 sCursorY = 0;
static EWRAM_DATA s16 sCenterX = 0;      // entry position, for the on-screen clamp
static EWRAM_DATA s16 sCenterY = 0;
static EWRAM_DATA u8 sWinId = 0;

// Brush mode (START cycles): paint a passable TILE, a solid WALL (same metatile +
// collision bit, so it blocks the player — zero sprite cost, like any tile), or
// place an OBJECT-event sprite (the only pool-limited layer).
enum { BRUSH_TILE, BRUSH_WALL, BRUSH_OBJECT, BRUSH_MODE_COUNT };
static EWRAM_DATA u8 sBrushMode = BRUSH_TILE;
static EWRAM_DATA u16 sTileSel = 0;      // raw metatile id being browsed (0..NUM_METATILES_TOTAL)
static EWRAM_DATA bool8 sTileInit = FALSE; // seeded to a sensible default on first entry
static EWRAM_DATA u8 sObjSel = 0;        // index into sObjPalette

// Eyedropper (A+B, tile/wall mode): pick the metatile under the cursor into the brush. The
// plain place/revert fire on button RELEASE and are swallowed once A+B has been held, so an
// almost-simultaneous A+B doesn't ALSO paint the cell (which would corrupt the sample).
static EWRAM_DATA bool8 sAHeldPrev = FALSE;
static EWRAM_DATA bool8 sBHeldPrev = FALSE;
static EWRAM_DATA bool8 sComboUsed = FALSE;

// ---- selectable palettes (named + previewable) ---------------------------

// Curated tiles from the GENERAL tileset (metatile ids < NUM_METATILES_IN_PRIMARY),
// which is loaded on every overworld map — so these render consistently everywhere,
// unlike the map-specific secondary tileset the old raw 0-1023 brush cycled into
// (which on most maps is a wall of grey interior metatiles). Names are kept short
// so the status strip fits.
struct TileDef { u16 id; const u8 *name; };
static const u8 sTn_Grass[]  = _("Grass");
static const u8 sTn_Tall[]   = _("TallGrs");
static const u8 sTn_Long[]   = _("LongGrs");
static const u8 sTn_Sand[]   = _("Sand");
static const u8 sTn_Water[]  = _("Water");
static const u8 sTn_Pond[]   = _("Pond");
static const u8 sTn_Ocean[]  = _("Ocean");
static const u8 sTn_Rock[]   = _("RockWall");
static const u8 sTn_Slope[]  = _("Slope");
static const struct TileDef sTilePalette[] = {
    { METATILE_General_Grass,           sTn_Grass },
    { METATILE_General_TallGrass,       sTn_Tall  },
    { METATILE_General_LongGrass,       sTn_Long  },
    { METATILE_General_SandPit_Center,  sTn_Sand  },
    { METATILE_General_CalmWater,       sTn_Water },
    { METATILE_General_ReflectiveWater, sTn_Pond  },
    { METATILE_General_RoughDeepWater,  sTn_Ocean },
    { METATILE_General_RockWall_RockBase, sTn_Rock },
    { METATILE_General_MuddySlope_Frame0, sTn_Slope },
};
#define TILE_PALETTE_COUNT ((u8)(sizeof(sTilePalette) / sizeof(sTilePalette[0])))

// Curated placeable object graphics (all verified in constants/event_objects.h).
// BERRY_TREE is deliberately excluded — its sprite reads per-object berry data that
// a spawned (template-less) object doesn't own, which can misbehave. Interaction
// with any of these is a safe no-op (see field_control_avatar.c: placed-object
// localIds skip the map-template script lookup).
static const u16 sObjPalette[] = {
    OBJ_EVENT_GFX_ITEM_BALL, OBJ_EVENT_GFX_CUTTABLE_TREE, OBJ_EVENT_GFX_BREAKABLE_ROCK,
    OBJ_EVENT_GFX_PUSHABLE_BOULDER, OBJ_EVENT_GFX_MAN_1, OBJ_EVENT_GFX_WOMAN_1,
    OBJ_EVENT_GFX_BOY_1, OBJ_EVENT_GFX_GIRL_1, OBJ_EVENT_GFX_LITTLE_BOY,
    OBJ_EVENT_GFX_LITTLE_GIRL, OBJ_EVENT_GFX_YOUNGSTER, OBJ_EVENT_GFX_FAT_MAN,
    OBJ_EVENT_GFX_GENTLEMAN, OBJ_EVENT_GFX_BEAUTY, OBJ_EVENT_GFX_FISHERMAN,
    OBJ_EVENT_GFX_HIKER, OBJ_EVENT_GFX_CAMPER, OBJ_EVENT_GFX_HEX_MANIAC,
    OBJ_EVENT_GFX_GAMEBOY_KID, OBJ_EVENT_GFX_CAMERAMAN, OBJ_EVENT_GFX_CLEFAIRY_DOLL,
    OBJ_EVENT_GFX_JIGGLYPUFF_DOLL, OBJ_EVENT_GFX_DITTO_DOLL, OBJ_EVENT_GFX_KECLEON_DOLL,
    OBJ_EVENT_GFX_BIG_SNORLAX_DOLL, OBJ_EVENT_GFX_BALL_CUSHION,
};
static const u8 sOn_Ball[]     = _("Ball");
static const u8 sOn_Tree[]     = _("Tree");
static const u8 sOn_Rock[]     = _("Rock");
static const u8 sOn_Boulder[]  = _("Boulder");
static const u8 sOn_Man[]      = _("Man");
static const u8 sOn_Woman[]    = _("Woman");
static const u8 sOn_Boy[]      = _("Boy");
static const u8 sOn_Girl[]     = _("Girl");
static const u8 sOn_LilBoy[]   = _("Lil Boy");
static const u8 sOn_LilGirl[]  = _("Lil Girl");
static const u8 sOn_Youth[]    = _("Youth");
static const u8 sOn_FatMan[]   = _("Fat Man");
static const u8 sOn_Gent[]     = _("Gentleman");
static const u8 sOn_Beauty[]   = _("Beauty");
static const u8 sOn_Fisher[]   = _("Fisher");
static const u8 sOn_Hiker[]    = _("Hiker");
static const u8 sOn_Camper[]   = _("Camper");
static const u8 sOn_Hex[]      = _("Hex Girl");
static const u8 sOn_GbaKid[]   = _("GBA Kid");
static const u8 sOn_Camera[]   = _("Cameraman");
static const u8 sOn_Clefairy[] = _("Clefairy");
static const u8 sOn_Jiggly[]   = _("Jiggly");
static const u8 sOn_Ditto[]    = _("Ditto");
static const u8 sOn_Kecleon[]  = _("Kecleon");
static const u8 sOn_Snorlax[]  = _("Snorlax");
static const u8 sOn_Cushion[]  = _("Cushion");
static const u8 *const sObjNames[] = {
    sOn_Ball, sOn_Tree, sOn_Rock, sOn_Boulder, sOn_Man, sOn_Woman, sOn_Boy,
    sOn_Girl, sOn_LilBoy, sOn_LilGirl, sOn_Youth, sOn_FatMan, sOn_Gent, sOn_Beauty,
    sOn_Fisher, sOn_Hiker, sOn_Camper, sOn_Hex, sOn_GbaKid, sOn_Camera, sOn_Clefairy,
    sOn_Jiggly, sOn_Ditto, sOn_Kecleon, sOn_Snorlax, sOn_Cushion,
};
#define OBJ_PALETTE_COUNT ((u8)(sizeof(sObjPalette) / sizeof(sObjPalette[0])))

// Placed objects live server-side (unlimited); the ROM holds the current map's
// list in EWRAM and renders only the ones near the player through a small pool of
// object-event slots (virtualization), so any number can be stored while the
// 16-slot save budget is never touched (see OBJECT_EVENTS_COUNT_SAVED).
#define MAX_PLACED_OBJS  48    // per-map list cap held in EWRAM
#define OBJ_POOL_SIZE    8     // concurrent on-screen object-event slots
#define OBJ_LOCALID_BASE 0xE0  // pool localIds 0xE0..0xE7 (below cursor 0xEF / remotes 0xF0+)
#define OBJ_RANGE_X      9     // spawn objects within this many tiles of the player
#define OBJ_RANGE_Y      7

struct PlacedObj { bool8 used; s16 lx; s16 ly; u16 gfx; }; // lx/ly are MAP-LOCAL
static EWRAM_DATA struct PlacedObj sObjs[MAX_PLACED_OBJS];
static EWRAM_DATA s16 sPoolObj[OBJ_POOL_SIZE]; // sObjs index each slot renders, or -1 (set in ResetObjects)
static EWRAM_DATA bool8 sObjDirty = FALSE;      // list changed → reconcile next tick
// Player tile at last reconcile. Zero-init here (BSS); ResetObjects sets it to
// -1 on the first map load, before any reconcile runs.
static EWRAM_DATA s16 sLastReconX = 0;
static EWRAM_DATA s16 sLastReconY = 0;

// Live cursor/preview state (see the cursor section).
static EWRAM_DATA bool8 sTilePrev = FALSE;   // a tile preview is drawn at (sCursorX,sCursorY)
static EWRAM_DATA u16 sTilePrevSaved = 0;    // grid entry that was there before the preview
static EWRAM_DATA bool8 sGhost = FALSE;      // a ghost object sprite is spawned (object mode)
static EWRAM_DATA u8 sCurSpriteId = 0; // the mode-tinted selection-box sprite (SPRITE_NONE set in Enter)
static EWRAM_DATA u8 sCursorGfx[128];        // 16x16 4bpp hollow box, generated at runtime
static EWRAM_DATA u16 sCursorPal[16];        // OBJ palette, recoloured per mode

static void Task_PeepoMapEdit(u8 taskId);
static void ClampCursor(void);

// True when the player's object event is live on the field (safe to touch the
// map grid / object-event system).
static bool8 InOverworld(void)
{
    return gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT
        && gObjectEvents[gPlayerAvatar.objectEventId].active
        && !gMain.inBattle;
}

// ---- packet send helpers -------------------------------------------------

static void SendEdit(s16 lx, s16 ly, u16 val, u8 flags)
{
    u8 pkt[10];
    pkt[0] = PKT_MAP_EDIT;
    pkt[1] = gSaveBlock1Ptr->location.mapGroup;
    pkt[2] = gSaveBlock1Ptr->location.mapNum;
    pkt[3] = lx & 0xFF;
    pkt[4] = (lx >> 8) & 0xFF;
    pkt[5] = ly & 0xFF;
    pkt[6] = (ly >> 8) & 0xFF;
    pkt[7] = val & 0xFF;
    pkt[8] = (val >> 8) & 0xFF;
    pkt[9] = flags;
    PeepoNet_Send(pkt, 10);
}

static void RequestSnapshot(u8 g, u8 n)
{
    u8 pkt[3];
    pkt[0] = PKT_MAP_REQ;
    pkt[1] = g;
    pkt[2] = n;
    PeepoNet_Send(pkt, 3);
}

static void SendObj(s16 lx, s16 ly, u16 gfx, u8 flags)
{
    u8 pkt[10];
    pkt[0] = PKT_MAP_OBJ;
    pkt[1] = gSaveBlock1Ptr->location.mapGroup;
    pkt[2] = gSaveBlock1Ptr->location.mapNum;
    pkt[3] = lx & 0xFF;
    pkt[4] = (lx >> 8) & 0xFF;
    pkt[5] = ly & 0xFF;
    pkt[6] = (ly >> 8) & 0xFF;
    pkt[7] = gfx & 0xFF;
    pkt[8] = (gfx >> 8) & 0xFF;
    pkt[9] = flags;
    PeepoNet_Send(pkt, 10);
}

// ---- protection ----------------------------------------------------------

// Any warp/coord/bg/object event sitting on this map-local cell.
static bool8 EventAt(s16 lx, s16 ly)
{
    const struct MapEvents *ev = gMapHeader.events;
    u32 i;
    if (ev == NULL)
        return FALSE;
    for (i = 0; i < ev->warpCount; i++)
        if (ev->warps[i].x == lx && ev->warps[i].y == ly)
            return TRUE;
    for (i = 0; i < ev->coordEventCount; i++)
        if (ev->coordEvents[i].x == lx && ev->coordEvents[i].y == ly)
            return TRUE;
    for (i = 0; i < ev->bgEventCount; i++)
        if (ev->bgEvents[i].x == lx && ev->bgEvents[i].y == ly)
            return TRUE;
    for (i = 0; i < ev->objectEventCount; i++)
        if (ev->objectEvents[i].x == lx && ev->objectEvents[i].y == ly)
            return TRUE;
    return FALSE;
}

// Untouchable: out-of-bounds, any event tile, or a warp/door/escalator/ladder
// metatile. Keeps transitions/portals/scripts intact.
static bool8 IsProtected(s16 lx, s16 ly)
{
    const struct MapLayout *ml = gMapHeader.mapLayout;
    s16 rx = lx + MAP_OFFSET, ry = ly + MAP_OFFSET;
    u8 b;

    if (ml == NULL || lx < 0 || ly < 0 || lx >= ml->width || ly >= ml->height)
        return TRUE;
    if (EventAt(lx, ly))
        return TRUE;

    b = MapGridGetMetatileBehaviorAt(rx, ry);
    if (MetatileBehavior_IsWarpDoor(b) || MetatileBehavior_IsDoor(b) || MetatileBehavior_IsNonAnimDoor(b)
        || MetatileBehavior_IsEscalator(b) || MetatileBehavior_IsLadder(b)
        || MetatileBehavior_IsNorthArrowWarp(b) || MetatileBehavior_IsSouthArrowWarp(b)
        || MetatileBehavior_IsEastArrowWarp(b) || MetatileBehavior_IsWestArrowWarp(b)
        || MetatileBehavior_IsDeepSouthWarp(b))
        return TRUE;
    return FALSE;
}

// ---- applying edits to the live map --------------------------------------

// Read the full runtime grid entry (metatile id + collision + elevation) at a
// runtime cell, for saving/restoring the preview.
static u16 GridEntryAt(s16 rx, s16 ry)
{
    if (rx < 0 || ry < 0 || rx >= gBackupMapLayout.width || ry >= gBackupMapLayout.height)
        return 0;
    return gBackupMapLayout.map[rx + gBackupMapLayout.width * ry];
}

// Write one map-local cell into the runtime map grid (no redraw). A delete
// restores the pristine metatile entry from the static map layout.
static void SetCell(s16 lx, s16 ly, u16 val, u8 flags)
{
    const struct MapLayout *ml = gMapHeader.mapLayout;
    s16 rx = lx + MAP_OFFSET, ry = ly + MAP_OFFSET;
    if (ml == NULL || lx < 0 || ly < 0 || lx >= ml->width || ly >= ml->height)
        return;
    if (flags & EDIT_DELETE)
        MapGridSetMetatileEntryAt(rx, ry, ml->map[ly * ml->width + lx]);
    else
        // Keep the collision bits too, so a placed WALL blocks on every client.
        MapGridSetMetatileIdAt(rx, ry, val & (MAPGRID_METATILE_ID_MASK | MAPGRID_COLLISION_MASK));
}

// ---- placed objects (virtualized rendering) ------------------------------

static s16 FindObjAt(s16 lx, s16 ly)
{
    u32 i;
    for (i = 0; i < MAX_PLACED_OBJS; i++)
        if (sObjs[i].used && sObjs[i].lx == lx && sObjs[i].ly == ly)
            return (s16)i;
    return -1;
}

static s16 AllocObj(void)
{
    u32 i;
    for (i = 0; i < MAX_PLACED_OBJS; i++)
        if (!sObjs[i].used)
            return (s16)i;
    return -1;
}

// Add/replace an object at a map-local cell (from local placement or the network).
static void SetObj(s16 lx, s16 ly, u16 gfx)
{
    s16 oi = FindObjAt(lx, ly);
    if (oi < 0)
        oi = AllocObj();
    if (oi < 0)
        return; // list full (>MAX_PLACED_OBJS on this map) — drop, best-effort
    sObjs[oi].used = TRUE;
    sObjs[oi].lx = lx;
    sObjs[oi].ly = ly;
    sObjs[oi].gfx = gfx;
    sObjDirty = TRUE;
}

static void DelObj(s16 lx, s16 ly)
{
    s16 oi = FindObjAt(lx, ly);
    if (oi >= 0)
    {
        sObjs[oi].used = FALSE;
        sObjDirty = TRUE;
    }
}

// Forget every placed object + free the render pool. The pool's object events
// are wiped by the map reload itself, so no despawn is needed here.
static void ResetObjects(void)
{
    u32 i;
    for (i = 0; i < MAX_PLACED_OBJS; i++)
        sObjs[i].used = FALSE;
    for (i = 0; i < OBJ_POOL_SIZE; i++)
        sPoolObj[i] = -1;
    sObjDirty = TRUE;
    sLastReconX = -1;
    sLastReconY = -1;
}

static bool8 InObjRange(s16 lx, s16 ly, s16 plx, s16 ply)
{
    s16 dx = lx - plx, dy = ly - ply;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= OBJ_RANGE_X && dy <= OBJ_RANGE_Y;
}

static s16 PoolSlotOfObj(s16 oi)
{
    u32 s;
    for (s = 0; s < OBJ_POOL_SIZE; s++)
        if (sPoolObj[s] == oi)
            return (s16)s;
    return -1;
}

static void DespawnPoolSlot(u32 slot)
{
    RemoveObjectEventByLocalIdAndMap(OBJ_LOCALID_BASE + slot,
        gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);
    sPoolObj[slot] = -1;
}

// Render the placed objects near the player through the fixed pool: despawn any
// slot whose object left range (or was deleted), then spawn in-range objects
// that lack a slot (until the pool fills). Static objects never move, so a
// spawned slot just sits there until it goes out of range.
static void ReconcileObjects(void)
{
    s16 px, py, plx, ply;
    u32 slot, i;
    if (!InOverworld())
        return;
    PlayerGetDestCoords(&px, &py);
    plx = px - MAP_OFFSET;
    ply = py - MAP_OFFSET;

    for (slot = 0; slot < OBJ_POOL_SIZE; slot++)
    {
        s16 oi = sPoolObj[slot];
        if (oi < 0)
            continue;
        if (!sObjs[oi].used || !InObjRange(sObjs[oi].lx, sObjs[oi].ly, plx, ply))
            DespawnPoolSlot(slot);
    }

    for (i = 0; i < MAX_PLACED_OBJS; i++)
    {
        u32 free;
        if (!sObjs[i].used || !InObjRange(sObjs[i].lx, sObjs[i].ly, plx, ply))
            continue;
        if (PoolSlotOfObj((s16)i) >= 0)
            continue; // already rendered
        for (free = 0; free < OBJ_POOL_SIZE && sPoolObj[free] >= 0; free++)
            ;
        if (free >= OBJ_POOL_SIZE)
            break; // pool full — remaining in-range objects don't render (best-effort)
        if (SpawnSpecialObjectEventParameterized(sObjs[i].gfx, MOVEMENT_TYPE_NONE,
                OBJ_LOCALID_BASE + free, sObjs[i].lx + MAP_OFFSET, sObjs[i].ly + MAP_OFFSET, 3)
            == OBJECT_EVENTS_COUNT)
            break; // spawn failed = GLOBAL object-event/sprite exhaustion, which can't
                   // recover inside this loop — stop rather than burn a failed attempt
                   // (palette + sheet load each) per remaining object. The slot stays
                   // unmapped, so the next reconcile (movement/edit/packet) retries.
        sPoolObj[free] = (s16)i;
    }
}

// ---- HM field moves on placed objects ------------------------------------
//
// Placed cuttable trees / breakable rocks / boulders respond to the real HM field
// moves (Cut, Rock Smash, Strength) — not to a plain A-press, so they behave like
// the map's own obstacles and need the badge + move. The field-move SETUP functions
// (reachable only by actually using the HM from the party menu) call
// PeepoMapEdit_PlacedFieldMoveTarget() to claim a placed object in front, then set
// PeepoMapEdit_DoFieldMovePlaced as the post-menu callback, which does the networked
// action. A networked delete/move is used (not just despawning the object event),
// because the placed-object list is what would otherwise respawn it each reconcile.

static EWRAM_DATA s16 sFMLx = 0;   // stashed target of the pending field move
static EWRAM_DATA s16 sFMLy = 0;
static EWRAM_DATA u16 sFMGfx = 0;
static EWRAM_DATA u8 sFMDir = 0;

// Claim a placed object with graphics `gfx` on the tile directly in front of the
// player. Returns TRUE (and stashes it) if there is one — the caller then wires up
// PeepoMapEdit_DoFieldMovePlaced. A real (map-owned) obstacle isn't in our list, so
// this returns FALSE for it and the vanilla field-move path handles it.
bool8 PeepoMapEdit_PlacedFieldMoveTarget(u16 gfx)
{
    s16 fx, fy, lx, ly, oi;

    if (!PeepoNet_Open() || gMapHeader.mapLayout == NULL)
        return FALSE;
    GetXYCoordsOneStepInFrontOfPlayer(&fx, &fy);
    lx = fx - MAP_OFFSET;
    ly = fy - MAP_OFFSET;
    oi = FindObjAt(lx, ly);
    if (oi < 0 || !sObjs[oi].used || sObjs[oi].gfx != gfx)
        return FALSE;
    sFMLx = lx;
    sFMLy = ly;
    sFMGfx = gfx;
    sFMDir = GetPlayerFacingDirection();
    return TRUE;
}

// Post-menu callback: apply the claimed field move. Cut/Rock Smash delete the object
// (networked, so it stays gone for everyone); Strength shoves a boulder one tile the
// way we're facing if that cell is free. The field is already unlocked here (the menu
// returned to the overworld), so no script/lock is needed — just act + play the SE.
void PeepoMapEdit_DoFieldMovePlaced(void)
{
    s16 oi = FindObjAt(sFMLx, sFMLy);
    if (oi < 0 || !sObjs[oi].used || sObjs[oi].gfx != sFMGfx)
        return; // the object moved/vanished since the menu opened

    if (sFMGfx == OBJ_EVENT_GFX_CUTTABLE_TREE || sFMGfx == OBJ_EVENT_GFX_BREAKABLE_ROCK)
    {
        DelObj(sFMLx, sFMLy);
        SendObj(sFMLx, sFMLy, 0, EDIT_DELETE);
        ReconcileObjects();
        PlaySE(sFMGfx == OBJ_EVENT_GFX_CUTTABLE_TREE ? SE_M_CUT : SE_M_ROCK_THROW);
    }
    else if (sFMGfx == OBJ_EVENT_GFX_PUSHABLE_BOULDER)
    {
        s16 nlx = sFMLx, nly = sFMLy;
        switch (sFMDir)
        {
        case DIR_NORTH: nly--; break;
        case DIR_SOUTH: nly++; break;
        case DIR_WEST:  nlx--; break;
        case DIR_EAST:  nlx++; break;
        default: return;
        }
        // Only shove onto a free, walkable, unprotected, object-less cell.
        if (!IsProtected(nlx, nly)
            && FindObjAt(nlx, nly) < 0
            && MapGridGetCollisionAt(nlx + MAP_OFFSET, nly + MAP_OFFSET) == 0)
        {
            s16 oi = FindObjAt(sFMLx, sFMLy);
            s16 slot;

            if (oi < 0)
                return;
            // Move the record IN PLACE — keeping both its sObjs index and its
            // render-pool slot. The old delete-then-re-add freed the index (so
            // the pool never moved the local object event: sprite AND collision
            // stayed at the source tile), and freeing the pool slot mid-move can
            // hand it to a 9th in-range object on a full pool, leaving the moved
            // boulder unrendered. The live object event just moves; the wire
            // protocol stays delete+add, which peers already reconcile per packet.
            sObjs[oi].lx = nlx;
            sObjs[oi].ly = nly;
            sObjDirty = TRUE;
            // KNOWN DEFERRED DEFECT (remote side): the wire has no MOVE opcode, so
            // peers see delete+add and reconcile between them — on a full render
            // pool the freed slot can go to a lower-indexed waiting object and the
            // moved boulder stays unrendered for that peer until range churn.
            // Add-first is no better (deterministically loses the same dense case,
            // and with all 48 records occupied the add is DROPPED, losing the
            // record outright — worse). Delete-first at least never loses the
            // record. A real fix is an atomic MOVE opcode, which requires a
            // server-side protocol change outside this repo.
            SendObj(sFMLx, sFMLy, 0, EDIT_DELETE);
            SendObj(nlx, nly, sFMGfx, 0);
            slot = PoolSlotOfObj(oi);
            if (slot >= 0) // rendered: move the live object event (takes map-local coords)
                TryMoveObjectEventToMapCoords(OBJ_LOCALID_BASE + slot,
                    gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup,
                    nlx, nly);
            ReconcileObjects(); // covers the not-currently-rendered case
            PlaySE(SE_M_STRENGTH);
        }
    }
}

// Can the player actually use this HM right now — badge obtained AND a (non-egg)
// party mon knows the move? Mirrors ScrCmd_checkfieldmove so A-press parity is gated
// exactly like the real obstacle scripts.
static bool8 PeepoCanUseFieldMove(enum FieldMove fm)
{
    u16 move;
    u32 i;
    if (!IsFieldMoveUnlocked(fm))
        return FALSE;
    move = FieldMove_GetMoveId(fm);
    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 species = GetMonData(&gPlayerParty[i], MON_DATA_SPECIES, NULL);
        if (!species)
            break;
        if (!GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG, NULL) && MonKnowsMove(&gPlayerParty[i], move) == TRUE)
            return TRUE;
    }
    return FALSE;
}

// A-press parity: pressing A facing a placed tree/rock/boulder does the same networked
// cut/smash/push as the HM field move — but only if the player can actually use that
// HM (so placed objects behave like real ones instead of a badge-free free-for-all).
// Called from the object-interaction resolver; returns TRUE if it acted (no dialog).
bool8 PeepoMapEdit_TryAPressFieldMove(u8 localId)
{
    u32 slot;
    s16 oi;
    u16 gfx;
    enum FieldMove fm;

    if (localId < OBJ_LOCALID_BASE || localId >= OBJ_LOCALID_BASE + OBJ_POOL_SIZE)
        return FALSE;
    slot = localId - OBJ_LOCALID_BASE;
    oi = sPoolObj[slot];
    if (oi < 0 || !sObjs[oi].used)
        return FALSE;
    gfx = sObjs[oi].gfx;

    if (gfx == OBJ_EVENT_GFX_CUTTABLE_TREE)         fm = FIELD_MOVE_CUT;
    else if (gfx == OBJ_EVENT_GFX_BREAKABLE_ROCK)   fm = FIELD_MOVE_ROCK_SMASH;
    else if (gfx == OBJ_EVENT_GFX_PUSHABLE_BOULDER) fm = FIELD_MOVE_STRENGTH;
    else return FALSE; // not an HM-interactable placed object

    if (!PeepoCanUseFieldMove(fm))
        return FALSE; // no badge / no party mon with the move → leave it be

    sFMLx = sObjs[oi].lx;
    sFMLy = sObjs[oi].ly;
    sFMGfx = gfx;
    sFMDir = GetPlayerFacingDirection();
    PeepoMapEdit_DoFieldMovePlaced();
    return TRUE;
}

// ---- inbound packets -----------------------------------------------------

// A remote/system write just landed on (rx,ry) (runtime coords). If the tile-brush
// preview sits on that exact cell, drop the preview claim: its saved restore-entry
// predates the write, and restoring it later would silently revert the committed
// tile AND collision for this client only (a lasting desync until the next
// snapshot). The preview re-shows on the next cursor step. Shared by BOTH inbound
// cell-write paths — single edits and snapshot replays.
static void InvalidatePreviewAt(s16 rx, s16 ry)
{
    if (sTilePrev && rx == sCursorX && ry == sCursorY)
        sTilePrev = FALSE;
}

static void HandleSnapshot(const u8 *p, u32 n)
{
    u8 g, mn, more, count;
    u32 i, off;
    if (n < 5 || !InOverworld())
        return;
    g = p[1];
    mn = p[2];
    more = p[3];
    count = p[4];
    if (g != gSaveBlock1Ptr->location.mapGroup || mn != gSaveBlock1Ptr->location.mapNum)
        return; // snapshot for a map we've since left — ignore

    off = 5;
    for (i = 0; i < count && off + 6 <= n; i++, off += 6)
    {
        s16 lx = (s16)(p[off] | (p[off + 1] << 8));
        s16 ly = (s16)(p[off + 2] | (p[off + 3] << 8));
        u16 val = p[off + 4] | (p[off + 5] << 8);
        SetCell(lx, ly, val, 0);
        InvalidatePreviewAt(lx + MAP_OFFSET, ly + MAP_OFFSET); // delayed snapshots hit previewed cells too
    }
    if (!more)
        DrawWholeMapView(); // redraw once the full snapshot is in
}

static void HandleRemoteEdit(const u8 *p, u32 n)
{
    u8 g, mn, flags;
    s16 lx, ly, rx, ry;
    u16 val;
    if (n < 10 || !InOverworld())
        return;
    g = p[1];
    mn = p[2];
    if (g != gSaveBlock1Ptr->location.mapGroup || mn != gSaveBlock1Ptr->location.mapNum)
        return;
    lx = (s16)(p[3] | (p[4] << 8));
    ly = (s16)(p[5] | (p[6] << 8));
    val = p[7] | (p[8] << 8);
    flags = p[9];
    SetCell(lx, ly, val, flags);
    rx = lx + MAP_OFFSET;
    ry = ly + MAP_OFFSET;
    InvalidatePreviewAt(rx, ry);
    CurrentMapDrawMetatileAt(rx, ry); // live single-tile redraw
}

static void HandleObjSnapshot(const u8 *p, u32 n)
{
    u8 g, mn, more, count;
    u32 i, off;
    if (n < 5 || !InOverworld())
        return;
    g = p[1];
    mn = p[2];
    more = p[3];
    count = p[4];
    if (g != gSaveBlock1Ptr->location.mapGroup || mn != gSaveBlock1Ptr->location.mapNum)
        return;
    off = 5;
    for (i = 0; i < count && off + 6 <= n; i++, off += 6)
    {
        s16 lx = (s16)(p[off] | (p[off + 1] << 8));
        s16 ly = (s16)(p[off + 2] | (p[off + 3] << 8));
        u16 gfx = p[off + 4] | (p[off + 5] << 8);
        SetObj(lx, ly, gfx);
    }
    if (!more)
        ReconcileObjects(); // render the newly-loaded objects near the player
}

static void HandleRemoteObj(const u8 *p, u32 n)
{
    u8 g, mn, flags;
    s16 lx, ly;
    u16 gfx;
    if (n < 10 || !InOverworld())
        return;
    g = p[1];
    mn = p[2];
    if (g != gSaveBlock1Ptr->location.mapGroup || mn != gSaveBlock1Ptr->location.mapNum)
        return;
    lx = (s16)(p[3] | (p[4] << 8));
    ly = (s16)(p[5] | (p[6] << 8));
    gfx = p[7] | (p[8] << 8);
    flags = p[9];
    if (flags & EDIT_DELETE)
        DelObj(lx, ly);
    else
        SetObj(lx, ly, gfx);
    ReconcileObjects();
}

void PeepoMapEdit_OnPacket(const u8 *p, u32 n)
{
    if (n < 1)
        return;
    if (p[0] == PKT_MAP_SNAP)
        HandleSnapshot(p, n);
    else if (p[0] == PKT_MAP_EDIT)
        HandleRemoteEdit(p, n);
    else if (p[0] == PKT_MAP_OBJ_SNAP)
        HandleObjSnapshot(p, n);
    else if (p[0] == PKT_MAP_OBJ)
        HandleRemoteObj(p, n);
}

// ---- per-frame tick ------------------------------------------------------

void PeepoMapEdit_Update(void)
{
    u8 g, n;
    s16 px, py, plx, ply;
    // gMapHeader.mapLayout can still be NULL for a window after the player object
    // goes active on first load — snapshots applied then are dropped by SetCell's
    // bounds check (it needs the layout), and the map isn't re-detected once it
    // loads. Wait for the layout so the request fires against a real, writable map.
    if (!PeepoNet_Open() || !InOverworld() || gMapHeader.mapLayout == NULL)
        return;
    g = gSaveBlock1Ptr->location.mapGroup;
    n = gSaveBlock1Ptr->location.mapNum;
    // A warp reloads the current map even when it lands back on the SAME map (same
    // group+num, different tile, or even the same tile via a self-looping warp): the
    // layout backup is rebuilt pristine and the old object events are wiped, exactly
    // like a cross-map warp, but g/n don't change, so the group/num check alone would
    // miss it and this map's persisted edits would silently vanish. ApplyCurrentWarp
    // bumps gPeepoWarpGen on every warp (menus/battles never do), so a change to the
    // counter flags a rebuild to re-pull. A counter (event) rather than a diff of
    // gLastUsedWarp (value), so back-to-back warps that leave the warp data
    // byte-identical are still caught.
    if (!sHaveMap || g != sMapGroup || n != sMapNum || sLastWarpGen != gPeepoWarpGen)
    {
        // Entered (or warped within) a map: the backup layout was rebuilt pristine,
        // so pull this map's persisted tiles AND placed objects and re-apply them
        // (they live only server-side). The old object events were wiped by the warp.
        sHaveMap = TRUE;
        sMapGroup = g;
        sMapNum = n;
        sLastWarpGen = gPeepoWarpGen;
        ResetObjects();
        RequestSnapshot(g, n); // triggers both MAP_SNAP (tiles) + MAP_OBJ_SNAP (objects)
        sSnapReqLeft = SNAP_REQ_TOTAL; // re-request a few more times as the map settles
        sSnapTimer = 0;
    }
    else if (sSnapReqLeft > 0 && ++sSnapTimer >= SNAP_REQ_FRAMES)
    {
        RequestSnapshot(g, n);
        sSnapReqLeft--;
        sSnapTimer = 0;
    }

    // Re-render placed objects when the player moves or the list changed, so the
    // pool always holds the ones nearest the player (bounded virtualization).
    PlayerGetDestCoords(&px, &py);
    plx = px - MAP_OFFSET;
    ply = py - MAP_OFFSET;
    if (sObjDirty || plx != sLastReconX || ply != sLastReconY)
    {
        ReconcileObjects();
        sObjDirty = FALSE;
        sLastReconX = plx;
        sLastReconY = ply;
    }
}

// ---- selection helpers ---------------------------------------------------

static const u8 *ModeName(void)
{
    static const u8 sM_Tile[] = _("TILE");
    static const u8 sM_Wall[] = _("WALL");
    static const u8 sM_Obj[]  = _("OBJ");
    return sBrushMode == BRUSH_OBJECT ? sM_Obj : (sBrushMode == BRUSH_WALL ? sM_Wall : sM_Tile);
}

// The tile brush browses the map's whole metatile range by raw id; sTilePalette is
// now just a NAME lookup for the well-known cross-map (general-tileset) tiles.
static const u8 *TileName(u16 id)
{
    u32 i;
    for (i = 0; i < TILE_PALETTE_COUNT; i++)
        if (sTilePalette[i].id == id)
            return sTilePalette[i].name;
    return NULL; // unnamed (map-specific) tile — the status shows its #id instead
}

// The metatile value (id + collision) the tile brush would place. Water auto-gets a
// collision bit so it blocks on-foot movement (its metatile already carries the
// surfable behavior — it just needs to be impassable so you Surf it, not walk it).
static u16 CurrentTileVal(void)
{
    u16 id = sTileSel & MAPGRID_METATILE_ID_MASK;
    u16 v = id;
    if (sBrushMode == BRUSH_WALL
        || MetatileBehavior_IsSurfableWaterOrUnderwater(UNPACK_BEHAVIOR(GetMetatileAttributesById(id))))
        v |= WALL_COLLISION;
    return v;
}

// ---- status strip --------------------------------------------------------

// A 3-line strip: mode + current selection, the neighbouring items in the list
// (so you can see what's next), and the control hints.
static const struct WindowTemplate sMapEditWindowTemplate = {
    .bg = 0,
    .tilemapLeft = 1,
    .tilemapTop = 1,
    .width = 16,
    .height = 6,
    .paletteNum = 15,
    .baseBlock = MAPEDIT_WIN_BASEBLOCK,
};

static void PrintLine(const u8 *str, u8 y)
{
    AddTextPrinterParameterized(sWinId, FONT_SMALL, str, 2, y, TEXT_SKIP_DRAW, NULL);
}

static void RefreshStatus(void)
{
    u8 buf[40];
    static const u8 sGap[] = _("  ");

    FillWindowPixelBuffer(sWinId, PIXEL_FILL(1));

    // Line 1: mode + selection — "OBJ  Ball" / "TILE  Grass" / "TILE  #234".
    StringCopy(buf, ModeName());
    StringAppend(buf, sGap);
    if (sBrushMode == BRUSH_OBJECT)
    {
        StringAppend(buf, sObjNames[sObjSel]);
    }
    else
    {
        const u8 *nm = TileName(sTileSel);
        if (nm != NULL)
        {
            StringAppend(buf, nm);
        }
        else
        {
            // Map-specific tile — no friendly name, so show its metatile number.
            ConvertIntToDecimalStringN(gStringVar1, sTileSel, STR_CONV_MODE_LEFT_ALIGN, 4);
            StringAppend(buf, gStringVar1);
        }
    }
    PrintLine(buf, 1);

    // Line 2: object mode shows what's next; tile mode shows the browse hint.
    if (sBrushMode == BRUSH_OBJECT)
    {
        static const u8 sNext[] = _("next  ");
        StringCopy(buf, sNext);
        StringAppend(buf, sObjNames[(sObjSel + 1) % OBJ_PALETTE_COUNT]);
        PrintLine(buf, 17);
    }
    else
    {
        static const u8 sBrowse[] = _("L R pick  hold fast");
        PrintLine(sBrowse, 17);
    }

    // Line 3: controls.
    {
        static const u8 sHint[] = _("A put  B erase  START");
        PrintLine(sHint, 33);
    }

    CopyWindowToVram(sWinId, COPYWIN_GFX);
}

static void DrawStatusWindow(void)
{
    sWinId = AddWindow(&sMapEditWindowTemplate);
    PutWindowTilemap(sWinId);
    DrawStdWindowFrame(sWinId, FALSE);
    RefreshStatus();
    CopyWindowToVram(sWinId, COPYWIN_MAP);
}

static void CloseStatusWindow(void)
{
    ClearStdWindowAndFrame(sWinId, TRUE);
    RemoveWindow(sWinId);
}

// ---- cursor + live preview -----------------------------------------------
// The cursor is a MODE-TINTED hollow selection box (green = tile, red = wall,
// blue = object) that always sits on the target cell — so it reads on any
// background AND shows the current mode at a glance (replacing the old pokeball,
// which looked like a placeable, and the blink-preview, which vanished when the
// tile matched its surroundings). Inside the box, the "content" previews what
// will be placed: tiles/walls draw the selected metatile into the cell; object
// mode spawns a ghost of the selected object sprite.
#define CURSOR_TILE_TAG 0x5EEC

// --- the selection-box sprite (its own OBJ palette, so it shows on any terrain) ---

static const struct OamData sCursorOam = {
    .affineMode = ST_OAM_AFFINE_OFF,
    .objMode = ST_OAM_OBJ_NORMAL,
    .bpp = ST_OAM_4BPP,
    .shape = SPRITE_SHAPE(16x16),
    .size = SPRITE_SIZE(16x16),
    .priority = 0,
};
static const union AnimCmd sCursorAnim0[] = { ANIMCMD_FRAME(0, 0), ANIMCMD_END };
static const union AnimCmd *const sCursorAnims[] = { sCursorAnim0 };
static const struct SpriteTemplate sCursorTemplate = {
    .tileTag = CURSOR_TILE_TAG,
    .paletteTag = CURSOR_PAL_TAG,
    .oam = &sCursorOam,
    .anims = sCursorAnims,
    .images = NULL,
    .affineAnims = gDummySpriteAffineAnimTable,
    .callback = SpriteCallbackDummy,
};

// Draw a 2px hollow box into the 16x16 4bpp frame (index 1 = the tint colour); the
// hollow interior lets the tile/object preview show through.
static void BuildCursorGfx(void)
{
    u32 px, py;
    for (px = 0; px < sizeof(sCursorGfx); px++)
        sCursorGfx[px] = 0;
    for (py = 0; py < 16; py++)
        for (px = 0; px < 16; px++)
        {
            u32 tile, lx, ly, off;
            if (px >= 2 && px <= 13 && py >= 2 && py <= 13)
                continue;
            tile = (px >= 8 ? 1 : 0) + (py >= 8 ? 2 : 0); // 16x16 = 2x2 of 8x8 tiles
            lx = px & 7;
            ly = py & 7;
            off = tile * 32 + ly * 4 + (lx >> 1);
            sCursorGfx[off] |= (lx & 1) ? 0x10 : 0x01; // set palette index 1
        }
}

static void CursorTint(void)
{
    u16 c = sBrushMode == BRUSH_WALL ? RGB(31, 8, 8)
          : (sBrushMode == BRUSH_OBJECT ? RGB(10, 14, 31) : RGB(12, 31, 14));
    u32 i, slot;
    sCursorPal[0] = RGB(0, 0, 0); // index 0 is transparent for OBJ regardless
    for (i = 1; i < 16; i++)
        sCursorPal[i] = c;
    slot = IndexOfSpritePaletteTag(CURSOR_PAL_TAG);
    if (slot != 0xFF)
        LoadPalette(sCursorPal, OBJ_PLTT_ID(slot), sizeof(sCursorPal));
}

// Glue the box to the current cursor cell (mirrors object-event screen mapping).
static void CursorSpriteMove(void)
{
    struct Sprite *s;
    if (sCurSpriteId == SPRITE_NONE)
        return;
    s = &gSprites[sCurSpriteId];
    SetSpritePosToMapCoords(sCursorX, sCursorY, &s->x, &s->y);
    s->centerToCornerVecX = -8;
    s->centerToCornerVecY = -8;
    s->x += 8;
    s->y += 8;
}

static void CursorSpriteCreate(void)
{
    struct SpriteSheet sheet = { sCursorGfx, sizeof(sCursorGfx), CURSOR_TILE_TAG };
    struct SpritePalette pal = { sCursorPal, CURSOR_PAL_TAG };
    u32 i;
    if (sCurSpriteId != SPRITE_NONE)
        return;
    BuildCursorGfx();
    for (i = 0; i < 16; i++)
        sCursorPal[i] = 0; // CursorTint fills the real colours after the palette loads
    LoadSpriteSheet(&sheet);
    LoadSpritePalette(&pal);
    sCurSpriteId = CreateSprite(&sCursorTemplate, 120, 80, 0);
    if (sCurSpriteId != SPRITE_NONE)
        // Track the map like object-event sprites do: the sprite system only adds
        // the camera scroll (gSpriteCoordOffset) when this is set, else the box
        // ignores the camera and lands at the screen origin (bottom-left).
        gSprites[sCurSpriteId].coordOffsetEnabled = TRUE;
    CursorTint();
    CursorSpriteMove();
}

static void CursorSpriteDestroy(void)
{
    if (sCurSpriteId != SPRITE_NONE)
    {
        DestroySprite(&gSprites[sCurSpriteId]);
        sCurSpriteId = SPRITE_NONE;
    }
    FreeSpriteTilesByTag(CURSOR_TILE_TAG);
    FreeSpritePaletteByTag(CURSOR_PAL_TAG);
}

// --- content preview under the box (tile draw / object ghost) ---

static void CursorContentHide(void)
{
    if (sTilePrev)
    {
        MapGridSetMetatileEntryAt(sCursorX, sCursorY, sTilePrevSaved);
        CurrentMapDrawMetatileAt(sCursorX, sCursorY);
        sTilePrev = FALSE;
    }
    if (sGhost)
    {
        RemoveObjectEventByLocalIdAndMap(CURSOR_LOCALID, gSaveBlock1Ptr->location.mapNum,
                                         gSaveBlock1Ptr->location.mapGroup);
        sGhost = FALSE;
    }
}

static void CursorContentShow(void)
{
    if (sBrushMode == BRUSH_OBJECT)
    {
        SpawnSpecialObjectEventParameterized(sObjPalette[sObjSel], MOVEMENT_TYPE_NONE,
            CURSOR_LOCALID, sCursorX, sCursorY, 3);
        sGhost = TRUE;
    }
    else
    {
        sTilePrevSaved = GridEntryAt(sCursorX, sCursorY);
        MapGridSetMetatileIdAt(sCursorX, sCursorY, CurrentTileVal());
        CurrentMapDrawMetatileAt(sCursorX, sCursorY);
        sTilePrev = TRUE;
    }
}

// Undo the cursor's content preview (called before leaving a cell / on exit).
static void CursorClear(void)
{
    CursorContentHide();
}

// Rebuild the content + retint the box for the current mode/selection.
static void CursorRefresh(void)
{
    CursorContentHide();
    CursorContentShow();
    CursorTint();
    CursorSpriteMove();
}

// Move the cursor one cell, keeping content + box with it.
static void CursorStep(s16 dx, s16 dy)
{
    CursorContentHide();
    sCursorX += dx;
    sCursorY += dy;
    ClampCursor();
    CursorContentShow();
    CursorSpriteMove();
}

// Clamp the cursor to the visible screen (around the entry point) and to the
// map's real bounds.
static void ClampCursor(void)
{
    const struct MapLayout *ml = gMapHeader.mapLayout;
    s16 minX = sCenterX - CURSOR_RANGE_X, maxX = sCenterX + CURSOR_RANGE_X;
    s16 minY = sCenterY - CURSOR_RANGE_Y, maxY = sCenterY + CURSOR_RANGE_Y;
    if (minX < MAP_OFFSET) minX = MAP_OFFSET;
    if (minY < MAP_OFFSET) minY = MAP_OFFSET;
    if (ml != NULL)
    {
        if (maxX > MAP_OFFSET + ml->width - 1) maxX = MAP_OFFSET + ml->width - 1;
        if (maxY > MAP_OFFSET + ml->height - 1) maxY = MAP_OFFSET + ml->height - 1;
    }
    if (sCursorX < minX) sCursorX = minX;
    if (sCursorX > maxX) sCursorX = maxX;
    if (sCursorY < minY) sCursorY = minY;
    if (sCursorY > maxY) sCursorY = maxY;
}

// ---- actions -------------------------------------------------------------

static void PlaceBrush(void)
{
    s16 lx = sCursorX - MAP_OFFSET, ly = sCursorY - MAP_OFFSET;
    u16 val;
    if (IsProtected(lx, ly))
    {
        PlaySE(SE_FAILURE);
        return;
    }
    val = CurrentTileVal();
    MapGridSetMetatileIdAt(sCursorX, sCursorY, val);
    CurrentMapDrawMetatileAt(sCursorX, sCursorY);
    // Commit the preview: the placed tile is now "what was there", so leaving the
    // cell (or the blink-off phase) keeps it instead of reverting.
    sTilePrevSaved = GridEntryAt(sCursorX, sCursorY);
    sTilePrev = TRUE;
    SendEdit(lx, ly, val, 0);
    PlaySE(SE_SELECT);
}

static void RevertBrush(void)
{
    s16 lx = sCursorX - MAP_OFFSET, ly = sCursorY - MAP_OFFSET;
    SetCell(lx, ly, 0, EDIT_DELETE);
    CurrentMapDrawMetatileAt(sCursorX, sCursorY);
    SendEdit(lx, ly, 0, EDIT_DELETE);
    // Re-show the brush preview over the now-cleared cell.
    sTilePrevSaved = GridEntryAt(sCursorX, sCursorY);
    MapGridSetMetatileIdAt(sCursorX, sCursorY, CurrentTileVal());
    CurrentMapDrawMetatileAt(sCursorX, sCursorY);
    sTilePrev = TRUE;
    PlaySE(SE_SELECT);
}

static void PlaceObject(void)
{
    s16 lx = sCursorX - MAP_OFFSET, ly = sCursorY - MAP_OFFSET;
    // Not on a warp/script tile, not on an existing object, and room in the list.
    if (IsProtected(lx, ly) || FindObjAt(lx, ly) >= 0 || AllocObj() < 0)
    {
        PlaySE(SE_FAILURE);
        return;
    }
    SetObj(lx, ly, sObjPalette[sObjSel]); // sets sObjDirty → reconcile spawns it next tick
    SendObj(lx, ly, sObjPalette[sObjSel], 0);
    PlaySE(SE_SELECT);
}

static void DeleteObject(void)
{
    s16 lx = sCursorX - MAP_OFFSET, ly = sCursorY - MAP_OFFSET;
    if (FindObjAt(lx, ly) < 0)
    {
        PlaySE(SE_FAILURE);
        return;
    }
    DelObj(lx, ly); // marks unused + dirty → reconcile despawns its pool slot
    SendObj(lx, ly, 0, EDIT_DELETE);
    PlaySE(SE_SELECT);
}

static void ChangeSel(s32 delta)
{
    if (sBrushMode == BRUSH_OBJECT)
    {
        // Objects: a small named list — wrap around.
        s32 s = (s32)sObjSel + delta;
        while (s < 0) s += OBJ_PALETTE_COUNT;
        sObjSel = (u8)(s % OBJ_PALETTE_COUNT);
    }
    else
    {
        // Tiles/Walls: browse the map's whole metatile range — clamp at the ends.
        s32 s = (s32)sTileSel + delta;
        if (s < 0) s = 0;
        if (s > NUM_METATILES_TOTAL - 1) s = NUM_METATILES_TOTAL - 1;
        sTileSel = (u16)s;
    }
    RefreshStatus();
    CursorRefresh(); // preview the newly-selected tile/object
    PlaySE(SE_SELECT);
}

static void CycleMode(void)
{
    sBrushMode = (u8)((sBrushMode + 1) % BRUSH_MODE_COUNT); // Tile -> Wall -> Object
    sAHeldPrev = FALSE; sBHeldPrev = FALSE; sComboUsed = TRUE; // drop any in-flight A/B combo
    RefreshStatus();
    CursorRefresh(); // rebuild the cursor for the new mode
    PlaySE(SE_SELECT);
}

static void ExitEditor(u8 taskId)
{
    CursorClear();
    CursorSpriteDestroy();
    CloseStatusWindow();
    ScriptUnfreezeObjectEvents();
    UnlockPlayerFieldControls();
    DestroyTask(taskId);
}

// Eyedropper: copy the metatile currently under the cursor into the tile brush, so you can
// paint more of it elsewhere. Reads the live grid entry, so it samples edits too, not just
// the original map. Only meaningful in TILE/WALL mode (objects aren't tiles).
static void EyedropUnderCursor(void)
{
    // The cursor draws a PREVIEW of the selected tile at its own cell, so the LIVE grid there
    // is that preview — NOT the real map tile. While the preview is up the real tile is stashed
    // in sTilePrevSaved, so read that; otherwise fall back to the live grid.
    u16 entry = sTilePrev ? sTilePrevSaved : (u16)MapGridGetMetatileIdAt(sCursorX, sCursorY);
    sTileSel = entry & MAPGRID_METATILE_ID_MASK;
    RefreshStatus();
    CursorRefresh();     // preview the freshly-picked tile inside the cursor box
    PlaySE(SE_SUCCESS);  // distinct "picked!" chime (vs SE_SELECT for browsing)
}

// A+B (tile/wall mode) = eyedropper. A and B each have their own tap action (place / revert),
// so to keep an almost-simultaneous A+B from ALSO editing the cell, the plain actions fire on
// button RELEASE and are swallowed for the rest of the press once A+B has been held together.
static void HandleTileBrushButtons(void)
{
    bool8 aHeld = (JOY_HELD(A_BUTTON) != 0);
    bool8 bHeld = (JOY_HELD(B_BUTTON) != 0);

    if (aHeld && bHeld)
    {
        if (!sComboUsed) { EyedropUnderCursor(); sComboUsed = TRUE; }
    }
    else
    {
        if (!sComboUsed && sAHeldPrev && !aHeld) PlaceBrush();  // A tapped alone -> place
        if (!sComboUsed && sBHeldPrev && !bHeld) RevertBrush(); // B tapped alone -> revert
        if (!aHeld && !bHeld) sComboUsed = FALSE;               // ready for the next combo
    }
    sAHeldPrev = aHeld;
    sBHeldPrev = bHeld;
}

static void HandleInput(u8 taskId)
{
    if (JOY_REPEAT(DPAD_UP))         { CursorStep(0, -1); }
    else if (JOY_REPEAT(DPAD_DOWN))  { CursorStep(0, +1); }
    else if (JOY_REPEAT(DPAD_LEFT))  { CursorStep(-1, 0); }
    else if (JOY_REPEAT(DPAD_RIGHT)) { CursorStep(+1, 0); }
    else if (JOY_NEW(START_BUTTON))  { CycleMode(); }                // Tile -> Wall -> Object
    // Tap = ±1 (precise); hold = fast (±8) so browsing the ~1000 tiles is bearable.
    // Objects are a short list, so a tap step is enough there.
    else if (JOY_NEW(R_BUTTON))      { ChangeSel(+1); }
    else if (JOY_NEW(L_BUTTON))      { ChangeSel(-1); }
    else if (JOY_REPEAT(R_BUTTON))   { ChangeSel(sBrushMode == BRUSH_OBJECT ? +1 : +8); }
    else if (JOY_REPEAT(L_BUTTON))   { ChangeSel(sBrushMode == BRUSH_OBJECT ? -1 : -8); }
    else if (JOY_NEW(SELECT_BUTTON)) { ExitEditor(taskId); return; }

    // A/B handled outside the else-if chain so the A+B eyedropper can watch both buttons
    // every frame. Object mode has no eyedropper — place/delete stay on immediate press.
    if (sBrushMode == BRUSH_OBJECT)
    {
        if (JOY_NEW(A_BUTTON))      PlaceObject();
        else if (JOY_NEW(B_BUTTON)) DeleteObject();
    }
    else
    {
        HandleTileBrushButtons();
    }
}

static void Task_PeepoMapEdit(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    switch (data[0])
    {
    case 0:
        DrawStatusWindow();
        CursorSpriteCreate(); // the mode-tinted selection box
        CursorRefresh();      // + the content preview inside it
        data[0] = 1;
        break;
    case 1:
        HandleInput(taskId);
        break;
    }
}

void PeepoMapEdit_Enter(void)
{
    s16 x, y;
    if (!InOverworld())
        return;
    PlayerGetDestCoords(&x, &y);
    sCursorX = sCenterX = x;
    sCursorY = sCenterY = y;
    sCurSpriteId = SPRITE_NONE; // EWRAM is zero-init, so arm the "no sprite" sentinel here
    sAHeldPrev = FALSE; sBHeldPrev = FALSE; sComboUsed = TRUE; // swallow the entry keypress release
    if (!sTileInit) // seed the tile brush to a friendly default (metatile 0 is blank)
    {
        sTileSel = METATILE_General_Grass;
        sTileInit = TRUE;
    }
    // The field is already frozen/locked by the QOL caller; draw the window and
    // build the cursor on the task's first step (next frame) to avoid touching
    // window VRAM in the middle of field-input processing.
    CreateTask(Task_PeepoMapEdit, 0x50);
}
