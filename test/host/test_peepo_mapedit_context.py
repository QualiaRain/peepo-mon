#!/usr/bin/env python3
"""Run the map-editor ingress functions on a host compiler with engine spies.

Usage: python3 test/host/test_peepo_mapedit_context.py [path/to/peepo_mapedit.c]
This checks packet/tick gating, not GBA rendering or a full ROM build.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "src/peepo_mapedit.c"
text = source.read_text()


def function(name):
    match = re.search(r"^(?:static )?(?:void|bool8) " + name + r"\([^;]*?\)\n\{", text, re.M)
    if match is None:
        raise ValueError(f"Function not found: {name}")
    return text[match.start():text.index("\n}", match.end()) + 2]


prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "constants/global.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef bool bool8;
#define TRUE true
#define FALSE false
#define MAP_OFFSET 7
#define PKT_MAP_EDIT 0x10
#define PKT_MAP_SNAP 0x12
#define PKT_MAP_OBJ 0x13
#define PKT_MAP_OBJ_SNAP 0x14
#define EDIT_DELETE 1
#define SNAP_REQ_TOTAL 2
#define SNAP_REQ_FRAMES 45
static void CB2_Overworld(void) {}
static void OtherScreen(void) {}
static struct { void (*callback2)(void); bool inBattle; } gMain;
static struct { u8 objectEventId; } gPlayerAvatar;
static struct { bool active; } gObjectEvents[OBJECT_EVENTS_COUNT];
static struct { void *mapLayout; } gMapHeader;
static struct Save { struct { u8 mapGroup, mapNum; } location; } save;
static struct Save *gSaveBlock1Ptr = &save;
static bool sHaveMap, sObjDirty;
static u8 sMapGroup, sMapNum, sSnapReqLeft;
static u16 sSnapTimer;
static s16 sLastReconX, sLastReconY;
static unsigned changes;
static bool PeepoNet_Open(void) { return true; }
static void SetCell(s16 x, s16 y, u16 v, u8 flags) { changes++; }
static void SetObj(s16 x, s16 y, u16 v) { changes++; }
static void DelObj(s16 x, s16 y) { changes++; }
static void InvalidatePreviewAt(s16 x, s16 y) { changes++; }
static void CurrentMapDrawMetatileAt(s16 x, s16 y) { changes++; }
static void DrawWholeMapView(void) { changes++; }
static void ReconcileObjects(void) { changes++; }
static void ResetObjects(void) { changes++; }
static void RequestSnapshot(u8 g, u8 n) { changes++; }
static void PlayerGetDestCoords(s16 *x, s16 *y) { *x = 7; *y = 7; }
'''

checks = r'''
int main(void)
{
    const u8 packets[][11] = {
        {PKT_MAP_EDIT, 0, 0, 1, 0, 1, 0, 1, 0, 0},
        {PKT_MAP_SNAP, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0},
        {PKT_MAP_OBJ, 0, 0, 1, 0, 1, 0, 1, 0, 0},
        {PKT_MAP_OBJ_SNAP, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0},
    };
    unsigned scenario, i;
    gMapHeader.mapLayout = &save;
    for (scenario = 0; scenario < 6; scenario++)
    {
        bool allowed = scenario == 0;
        gMain.callback2 = scenario == 1 ? OtherScreen : CB2_Overworld;
        if (scenario == 2) gMain.callback2 = NULL;
        gMain.inBattle = scenario == 3;
        gPlayerAvatar.objectEventId = scenario == 5 ? OBJECT_EVENTS_COUNT : 0;
        gObjectEvents[0].active = scenario != 4;
        for (i = 0; i < sizeof(packets) / sizeof(packets[0]); i++)
        {
            changes = 0;
            PeepoMapEdit_OnPacket(packets[i], sizeof(packets[i]));
            if ((changes != 0) != allowed)
            {
                fprintf(stderr, "packet %u: scenario %u changed engine state %u times\n", i, scenario, changes);
                return 1;
            }
        }
        changes = 0;
        sHaveMap = false;
        PeepoMapEdit_Update();
        assert((changes != 0) == allowed);
    }
    puts("PASS: four packet paths and frame tick reject stale/non-field state; live field remains active");
}
'''

names = ("InOverworld", "HandleSnapshot", "HandleRemoteEdit", "HandleObjSnapshot",
         "HandleRemoteObj", "PeepoMapEdit_OnPacket", "PeepoMapEdit_Update")
with tempfile.TemporaryDirectory() as tmp:
    c_file = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    c_file.write_text(prelude + "\n".join(function(name) for name in names) + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-iquote", str(ROOT / "include"), str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
