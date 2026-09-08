#!/usr/bin/env python3
"""Exercise the production POS parser using a host compiler, without a ROM.

Usage: python3 test/host/test_peepo_remote_direction.py [path/to/peepo_overworld.c]
The harness uses the real parser, roster, and movement queue functions.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "src/peepo_overworld.c"
text = source.read_text()


def function(name):
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\)\n\{", text, re.M)
    if match is None:
        raise ValueError(f"Function not found: {name}")
    return text[match.start():text.index("\n}", match.end()) + 2]


prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "constants/global.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef bool bool8;
#define TRUE true
#define FALSE false
#define EWRAM_DATA
'''
definitions = "\n".join(re.findall(
    r"^#define (?:MAX_REMOTES|MQ_SIZE|POS_CORE_LEN|POS_LEN)\s+[^\n]*", text, re.M))
struct = re.search(r"^struct RemotePlayer\n\{.*?^};", text, re.M | re.S).group()
state = "\n".join(re.findall(
    r"^static EWRAM_DATA [^\n]*\b(?:sRemotes|sHasLocalId|sLocalId|sFrame)\b[^\n]*", text, re.M))
queue_resets = "\n".join(re.findall(r"^static void (?:MqClear|FmqClear)\([^\n]*", text, re.M))
checks = r'''
static void checkDirection(unsigned direction, unsigned length, bool existing)
{
    u8 packet[POS_LEN] = {1, 2, 3, 4, 10, 0, 11, 0, 0, FEMALE, 25, 0, 1, 0x34, 0x92};
    struct RemotePlayer *r;
    unsigned expected = direction >= DIR_SOUTH && direction <= DIR_EAST ? direction : DIR_SOUTH;
    memset(sRemotes, 0, sizeof(sRemotes));
    sFrame = 42;
    sHasLocalId = false;
    if (existing)
    {
        packet[8] = DIR_NORTH;
        HandlePos(packet, length);
    }
    packet[8] = direction;
    HandlePos(packet, length);
    r = FindRemote(2);
    assert(r != NULL);
    if (r->dir != expected)
    {
        fprintf(stderr, "direction %u escaped ingress as %u (expected %u, length %u, existing %u)\n",
                direction, r->dir, expected, length, existing);
        assert(r->dir == expected);
    }
    assert(r->x == 10 && r->y == 11 && r->mapGroup == 3 && r->mapNum == 4);
    assert(r->gender == FEMALE && r->lastSeen == 42);
    assert(r->followSpecies == (length >= 13 ? 25 : 0));
    assert(r->followFlags == (length >= 13 ? 1 : 0));
    assert(r->accent == (length >= 15 ? 0x9234 : 0));
}

int main(void)
{
    unsigned direction, existing, i;
    const unsigned lengths[] = {POS_CORE_LEN, POS_CORE_LEN + 3, POS_LEN};
    u8 packet[POS_LEN] = {1, 2, 3, 4, 10, 0, 11, 0, 9, FEMALE};
    checkDirection(9, POS_CORE_LEN, false); // the engine's one-past-end lookup case
    for (direction = 0; direction <= UINT8_MAX; direction++)
        for (existing = 0; existing < 2; existing++)
            for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
                checkDirection(direction, lengths[i], existing);
    memset(sRemotes, 0, sizeof(sRemotes));
    HandlePos(packet, POS_CORE_LEN - 1);
    assert(FindRemote(2) == NULL);
    sHasLocalId = true;
    sLocalId = 2;
    HandlePos(packet, POS_LEN);
    assert(FindRemote(2) == NULL);
    puts("PASS: all 256 directions, three packet lengths, new/existing peers, short packets and self echoes");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c_file = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    c_file.write_text("\n".join([prelude, definitions, struct, state, queue_resets,
                                *(function(name) for name in ("FindRemote", "AllocRemote", "MqPush", "HandlePos")), checks]))
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all", "-iquote", str(ROOT / "include"),
                    str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
