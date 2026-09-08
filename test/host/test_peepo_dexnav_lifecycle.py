#!/usr/bin/env python3
"""Host checks for DexNav teardown; requires Python 3 and a C compiler with UBSan.

Run from any directory. --source-ref REV tests the same cases against an older
git revision, e.g. --source-ref 3c4fd7b2 reproduces the pre-fix failures.

The harness extracts production reset, end, reveal, graphics teardown and task
lookup/destruction functions, plus the actual search-to-reveal transition.
Graphics, flags and allocation use ownership-checking host doubles. This tests
the lifecycle, not the full search loop, GBA graphics, ROM build or gameplay.
"""

import argparse
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def block(source, start):
    """Return a complete braced C block, ignoring comments and strings."""
    masked = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match[0]), source, flags=re.S,
    )
    opening = masked.index("{", start)
    depth = 0
    for end in range(opening, len(masked)):
        depth += (masked[end] == "{") - (masked[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError("Unclosed C block")


def function(source, name):
    match = re.search(
        rf"(?m)^(?:static )?(?:void|u8) {name}\([^;{{}}]*\)\s*\{{", source,
    )
    if match is None:
        raise ValueError(f"Cannot find production definition: {name}")
    return block(source, match.start())


PREAMBLE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint8_t bool8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
#define TRUE 1
#define FALSE 0
#define MAX_MON_MOVES 4
#define MAX_SPRITES 64
#define NELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define FLAG_GET_SEEN 1
#define FLAG_GET_CAUGHT 2
#define R_BUTTON 1
#define SE_DEX_SEARCH 1
#define JOY_NEW(button) (revealButton)
"""


DOUBLES = r"""
static struct DexNavSearch *sDexNavSearchDataPtr;
static struct DexNavSearch *allocation;
struct Task gTasks[NUM_TASKS];
static struct { u8 dexNavChain; } saveBlock;
static typeof(saveBlock) *gSaveBlock3Ptr = &saveBlock;
static struct SpriteTemplate { u16 paletteTag; } iconTemplate;
static struct Sprite { bool8 inUse; struct SpriteTemplate *template; } gSprites[MAX_SPRITES];
static u16 gPlttBufferUnfaded[512];
static bool8 searching, windowLive, revealButton, seen;
static u16 stepCounter;
static unsigned frees, windowRemovals, spriteRemovals, effectsStopped, redraws;
static unsigned tileFrees, paletteFrees, monPaletteFrees, sounds;

static void Task_DexNavSearch(u8 taskId);
static void Task_RevealHiddenMon(u8 taskId);
static void UnrelatedTask(u8 taskId) { (void)taskId; }

static bool8 FlagGet(u16 flag) { assert(flag == DN_FLAG_SEARCHING); return searching; }
static void FlagClear(u16 flag) { assert(flag == DN_FLAG_SEARCHING); searching = FALSE; }
static void VarSet(u16 var, u16 value) { assert(var == DN_VAR_STEP_COUNTER); stepCounter = value; }
void Free(void *pointer)
{
    assert(pointer != NULL && pointer == allocation);
    frees++;
    free(pointer);
    allocation = NULL;
}
static void DestroySprite(struct Sprite *sprite)
{
    assert(sprite >= gSprites && sprite < gSprites + MAX_SPRITES);
    assert(sprite->inUse); /* Detect double teardown and release of unrelated sprites. */
    sprite->inUse = FALSE;
    spriteRemovals++;
}
static void ClearStdWindowAndFrameToTransparent(u8 windowId, bool8 copy)
{
    assert(windowId == 4 && windowLive);
}
static void CopyWindowToVram(u8 windowId, u8 mode) { assert(windowId == 4 && windowLive); }
static void RemoveWindow(u8 windowId)
{
    assert(windowId == 4 && windowLive);
    windowLive = FALSE;
    windowRemovals++;
}
static void FieldEffectStop(struct Sprite *sprite, u8 effect)
{
    assert(sprite == &gSprites[9] && effect == 1);
    DestroySprite(sprite);
    effectsStopped++;
}
static void FreeSpriteTilesByTag(u16 tag) { tileFrees++; }
static void FreeSpritePaletteByTag(u16 tag) { paletteFrees++; }
static void SafeFreeMonIconPalette(u16 species) { assert(species == 25); monPaletteFrees++; }
static void PlaySE(u16 sound) { assert(sound == SE_DEX_SEARCH); sounds++; }
static u16 SpeciesToNationalPokedexNum(u16 species) { return species; }
static bool8 GetSetPokedexFlag(u16 species, u8 flag) { return flag == FLAG_GET_SEEN ? seen : TRUE; }
static void DrawSearchWindow(u16 species, u8 potential, bool8 hidden)
{
    assert(!windowLive);
    assert(hidden == !seen);
    sDexNavSearchDataPtr->windowId = 4;
    windowLive = TRUE;
    redraws++;
}
static void DrawDexNavSearchMonIcon(u16 species, u8 *spriteId, bool8 owned)
{
    assert(windowLive && !gSprites[1].inUse);
    *spriteId = 1;
    gSprites[1].inUse = TRUE;
    gSprites[1].template = &iconTemplate;
    if (owned)
    {
        assert(!gSprites[7].inUse);
        sDexNavSearchDataPtr->ownedIconSpriteId = 7;
        gSprites[7].inUse = TRUE;
    }
}
static u8 IndexOfSpritePaletteTag(u16 tag) { return 0; }
static void CpuCopy16(const void *src, void *dst, u32 size) { memcpy(dst, src, size); }
static void TintPalette_CustomTone(u16 *palette, u16 count, u16 r, u16 g, u16 b) {}
static void LoadPalette(const void *palette, u16 offset, u16 size) {}
static void DexNavUpdateDirectionArrow(void) { assert(windowLive); }
"""


CASES = r"""
static void init(bool8 activeSearch, bool8 hidden)
{
    searching = TRUE;
    stepCounter = 37;
    saveBlock.dexNavChain = 19;
    gTasks[7].isActive = TRUE;
    gTasks[7].func = UnrelatedTask;
    gTasks[7].prev = activeSearch ? 2 : HEAD_SENTINEL;
    gTasks[7].next = TAIL_SENTINEL;
    if (!activeSearch)
        return;

    allocation = calloc(1, sizeof(*allocation));
    assert(allocation != NULL);
    sDexNavSearchDataPtr = allocation;
    allocation->species = 25;
    allocation->hiddenSearch = hidden;
    allocation->proximity = CREEPING_PROXIMITY;
    allocation->windowId = 4;
    allocation->iconSpriteId = 1;
    allocation->itemSpriteId = hidden ? MAX_SPRITES : 2;
    allocation->eyeSpriteId = hidden ? MAX_SPRITES : 3;
    allocation->starSpriteIds[0] = hidden ? MAX_SPRITES : 4;
    allocation->starSpriteIds[1] = hidden ? MAX_SPRITES : 5;
    allocation->starSpriteIds[2] = hidden ? MAX_SPRITES : 6;
    allocation->ownedIconSpriteId = 7;
    allocation->exclamationSpriteId = hidden ? 8 : MAX_SPRITES;
    allocation->fldEffSpriteId = 9;
    allocation->fldEffId = 1;
    for (u8 i = 1; i <= 9; i++)
        gSprites[i].inUse = hidden ? (i == 1 || i >= 7) : (i != 8);
    windowLive = TRUE;
    gTasks[2].isActive = TRUE;
    gTasks[2].func = Task_DexNavSearch;
    gTasks[2].prev = HEAD_SENTINEL;
    gTasks[2].next = 7;
    gTasks[2].tRevealed = !hidden;
    gTasks[2].tFrameCount = 81;
    seen = TRUE;
}

static void assertReset(void)
{
    assert(!searching && stepCounter == 0 && saveBlock.dexNavChain == 0);
    assert(!gTasks[2].isActive);
    assert(gTasks[7].isActive && gTasks[7].func == UnrelatedTask);
    assert(gTasks[7].prev == HEAD_SENTINEL && gTasks[7].next == TAIL_SENTINEL);
}

static void assertReleased(void)
{
    assertReset();
    assert(frees == 1 && allocation == NULL && sDexNavSearchDataPtr == NULL);
    assert(!windowLive && effectsStopped == 1);
    assert(tileFrees == 5 && paletteFrees == 1 && monPaletteFrees == 1);
    for (u8 i = 1; i <= 9; i++)
        assert(!gSprites[i].inUse);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *test = argv[1];
    if (!strcmp(test, "normal_search") || !strcmp(test, "hidden_search"))
    {
        bool8 hidden = !strcmp(test, "hidden_search");
        init(TRUE, hidden);
        ResetDexNavSearch();
        assertReleased();
        assert(windowRemovals == 1 && spriteRemovals == (hidden ? 4u : 8u));
    }
    else if (!strcmp(test, "reveal_reset") || !strcmp(test, "transition_ownership")
          || !strcmp(test, "reveal_seen") || !strcmp(test, "reveal_unseen"))
    {
        init(TRUE, TRUE);
        revealButton = TRUE;
        Task_DexNavSearch(2); /* Actual production transition, before the next task frame. */
        assert(gTasks[2].func == Task_RevealHiddenMon && gTasks[2].tRevealed);
        assert(sounds == 1);
        if (!strcmp(test, "transition_ownership"))
            assert(windowLive && gSprites[1].inUse && windowRemovals == 0);
        if (!strcmp(test, "reveal_seen") || !strcmp(test, "reveal_unseen"))
        {
            seen = !strcmp(test, "reveal_seen");
            gTasks[2].func(2);
            assert(gTasks[2].func == Task_DexNavSearch && gTasks[2].tFrameCount == 0);
            assert(windowLive && redraws == 1 && windowRemovals == 1);
        }
        ResetDexNavSearch();
        assertReleased();
        assert(windowRemovals == (redraws ? 2u : 1u));
        assert(spriteRemovals == (redraws ? (seen ? 6u : 5u) : 4u));
        ResetDexNavSearch(); /* Must be idempotent. */
        assertReleased();
    }
    else if (!strcmp(test, "stale_flag_null") || !strcmp(test, "stale_flag_pointer")
          || !strcmp(test, "idle_reset"))
    {
        init(FALSE, FALSE);
        if (strcmp(test, "stale_flag_null"))
            sDexNavSearchDataPtr = (struct DexNavSearch *)(uintptr_t)1;
        struct DexNavSearch *previous = sDexNavSearchDataPtr;
        if (!strcmp(test, "idle_reset"))
            searching = FALSE;
        ResetDexNavSearch();
        assertReset();
        assert(sDexNavSearchDataPtr == previous);
        assert(frees == 0 && windowRemovals == 0 && spriteRemovals == 0);
        assert(effectsStopped == 0 && tileFrees == 0 && paletteFrees == 0 && monPaletteFrees == 0);
        ResetDexNavSearch();
        assertReset();
    }
    else
        assert(!"Unknown test");
    puts("PASS");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="read production C and headers from a git revision")
    args = parser.parse_args()

    def read(path):
        if args.source_ref:
            return subprocess.check_output(
                ["git", "show", f"{args.source_ref}:{path}"], cwd=ROOT, text=True,
            )
        return (ROOT / path).read_text()

    dexnav = read("src/dexnav.c")
    task = read("src/task.c")
    structure = block(dexnav, dexnav.index("struct DexNavSearch\n{")) + ";"
    task_aliases = "\n".join(re.findall(r"(?m)^#define t\w+ +data\[\d+\]", dexnav))
    search = function(dexnav, "Task_DexNavSearch")
    transition_start = search.index("if (sDexNavSearchDataPtr->hiddenSearch && !task->tRevealed &&")
    transition = block(search, transition_start)
    selected_search = (
        "static void Task_DexNavSearch(u8 taskId) {\n"
        "struct Task *task = &gTasks[taskId];\n" + transition + "\n}"
    )
    source = "\n\n".join([
        PREAMBLE,
        read("include/task.h"), read("include/malloc.h"),
        read("include/config/dexnav.h"),
        read("include/dexnav.h").replace('#include "config/dexnav.h"', ""),
        structure, task_aliases, DOUBLES,
        function(task, "FindTaskIdByFunc"), function(task, "DestroyTask"),
        function(dexnav, "RemoveDexNavWindowAndGfx"),
        function(dexnav, "EndDexNavSearch"),
        function(dexnav, "Task_RevealHiddenMon"), selected_search,
        function(dexnav, "ResetDexNavSearch"), CASES,
    ])
    tests = [
        "normal_search", "hidden_search", "reveal_reset", "transition_ownership",
        "reveal_seen", "reveal_unseen", "stale_flag_null", "stale_flag_pointer", "idle_reset",
    ]
    # Baseline assertions/UBSan deliberately fail; avoid leaving core dumps.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="peepo-dexnav-lifecycle-") as directory:
        directory = Path(directory)
        c_file, binary = directory / "lifecycle.c", directory / "lifecycle"
        c_file.write_text(source)
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
            "-g", str(c_file), "-o", str(binary),
        ], check=True)
        failures = 0
        for test in tests:
            result = subprocess.run([str(binary), test], text=True, capture_output=True)
            print(f"{'FAIL' if result.returncode else 'PASS'} {test}")
            if result.returncode:
                failures += 1
                print(result.stderr.strip())
        print(f"{len(tests) - failures}/{len(tests)} passed (host lifecycle harness, UBSan)")
        return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
