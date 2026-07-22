# Crash / hang telemetry — design (for review)

**Goal:** when a player's game crashes or freezes, the developer gets a diagnostic report automatically. The player does not reproduce it, describe it, or even need to know it happened.

**Status:** design for review (Claude + Codex/Sol). No code written yet. All repo anchors verified against the current `peepo` tree.

## Platform constraints that shape the design

1. **A hard fault cannot be caught inside the ROM.** The ARM undefined-instruction and data-abort vectors are in the GBA BIOS mask ROM (`0x00`-`0x24`) and cannot be overridden by a cartridge; only the IRQ vector is BIOS-redirectable (`src/main.c:319`, `INTR_VECTOR`). There is no crash-screen infrastructure in this fork, and one is not addable for genuine hardware faults. **Hard-fault capture must therefore live in the customized mGBA, not the ROM.**
2. **The net transport is emulator-only.** `PeepoNet_*` talks to a patched mGBA fork's MMIO mailbox, which bridges to an out-of-repo room server (Cloudflare Durable Object) — `include/peepo_net.h:4`, `src/peepo_net.c:1`. It is not the SIO link cable. A network crash report only reaches a server for players on that patched build.
3. **But the emulator is exactly what we have.** Players run a customized mGBA (confirmed). The emulator survives ROM crashes, already holds the network channel and the player's identity, and can read GBA RAM directly. It is the ideal crash reporter. The ROM's job is to leave it a good record to find.
4. **A debug-log channel already ships.** `DebugPrintf` / `MgbaPrintf` (`include/gba/isagbprint.h:33`) is compiled into normal `MODERN=1` builds (stripped only under `RELEASE=1`, which CI does not set — `include/config/general.h:11`). This is a second, already-live line to the emulator, separate from the net mailbox.

## Architecture: a three-layer flight recorder

| Layer | Owner | Responsibility |
|---|---|---|
| **ROM** | us (this repo) | Maintain a live "black box" in fixed RAM; convert known crash points into guard-sensors that record a reason and recover; keep a heartbeat; best-effort self-report of soft faults. |
| **Customized mGBA** | friend's side | Watchdog the heartbeat; trap on the assert opcode / invalid access; harvest the black box from RAM; forward it. |
| **Room server (DO)** | friend's side | Receive the report, stamp it with player + time, log it. |

The ROM half is fully ours to build. The other two halves are small, precisely-specified interfaces (below). Hard crashes are caught by the emulator harvesting the black box; soft/known faults are caught and reported by the ROM itself. Between them they cover both crash classes.

## ROM-side specification

### 1. The black box (fixed-address RAM struct)

A single global, `struct PeepoBlackBox sPeepoBlackBox`, updated continuously during play. Layout (compact, fits well under 256 bytes):

- `magic` (u32/u64) — a fixed tag the emulator scans EWRAM for, so it never depends on the linker address. Version byte + struct length follow the magic.
- `heartbeat` (u32) — incremented once per main-loop iteration in `AgbMainLoop` (`src/main.c:137`). The emulator watchdogs this.
- `frame` (u32) — snapshot of `gMain.vblankCounter1` (`src/main.c:370`).
- `mapGroup` / `mapNum` (u8, u8) — current map.
- `callback2` (u32) — `gMain.callback2` pointer (identifies the active game state).
- `activeTask` (u32) — current script/DexNav/battle task func where applicable.
- `faultReason` (u8) + `faultDetail` (u16/u32) — set by a guard-sensor when one trips (0 = running normally).
- `pktRing[N]` — ring of the last N net-packet opcodes seen in `PeepoOverworld_Update`'s poll loop (`src/peepo_overworld.c:912`), each with a direction bit. This is the single most diagnostic field for the netcode bugs.
- `scriptRing[N]` — ring of the last N script/`RunScriptImmediately` contexts (catches the synchronous-script hang class).

**Address discovery:** emulator scans EWRAM for `magic`. Robust to rebuilds. Fallback: the build also emits the symbol address to the `.map`, which the emulator can read.

### 2. Guard-sensors (every known crash becomes catch + record + recover)

The elegant property: each guard both prevents a crash and reports it. Concrete first targets, all already-known defects:

- **Peer-name recursion** (`StringExpandPlaceholders`, `src/string_util.c:371`; template at `data/event_scripts.s` `PeepoInteractText`). Add a recursion-depth guard: on exceeding depth, stop expanding, set `faultReason = FAULT_STR_RECURSION` + the offending name, and return safely instead of overflowing EWRAM. Prevents the hard-lock **and** reports the culprit name.
- **DexNav seam UAF** (`src/dexnav.c`, PR #15 territory): the sentinel/`TASK_NONE` checks record `FAULT_DEXNAV_SEAM` when they fire.
- **Hostile packet fields** (map-id / dir / coord / species validators in `src/peepo_overworld.c`, PRs #19/#22/#23/#24): each rejection records `FAULT_BAD_PACKET` + opcode + peer, so a peer sending garbage is visible.
- **Heap guards**: the existing `AGB_ASSERT` sites in `src/malloc.c:108,122` already trap; add a black-box write immediately before the trap opcode so the emulator's harvest has context.

### 3. Hang watchdog

Primary: the emulator watchdogs `heartbeat` (it stops advancing on any main-loop hang, while `gMain.vblankCounter1` keeps ticking — the two diverging *is* the hang signal). Optional ROM backup: a check inside `VBlankIntr` (`src/main.c:363`, still runs during a main-loop hang) that, after K stalled frames, sets `faultReason = FAULT_HANG` in the black box.

### 4. Self-report channels (soft faults only)

When a guard trips on a recoverable path, the ROM opportunistically emits both:
- `DebugPrintf(MGBA_LOG_ERROR, "PEEPOCRASH r=%d d=%08x map=%d,%d ...")` — already-live channel, zero new server work.
- `PKT_CRASH` (new opcode `0x08`; `0x02` is also free) via `PeepoNet_Send` (`src/peepo_net.c:24`), payload = the black box, `<= NET_MAX` (256).

Hard faults skip this (the ROM is dead) and rely on the emulator harvest.

### 5. Optional: survive a reset

For soft faults that reach save code, frame-spread-persist the black box into the inert 104-byte `struct Pokedex.filler` at SaveBlock2 `0x28` (`include/global.h:274`, "Previously Dex Flags, feel free to remove," gated `FREE_EXTRA_SEEN_FLAGS_SAVEBLOCK2 == FALSE`) using the non-freezing `Task_PeepoBatterySave` pattern (`src/peepo_overworld.c:31`). Sent on next connect. **Adds an entry to the CLAUDE.md upstream-bump hand-audit gate** (same filler-reclaim hazard class as `peepoRandoFlags`).

## What the friend's side must do (the only external dependencies)

1. **Customized mGBA:** (a) poll the black-box `heartbeat`; declare a hang if it stalls while frames advance; (b) trap the assert opcode (`.hword 0xEFFF`, `src/libisagbprn.c:203`) and invalid-memory access; (c) on either, scan EWRAM for `magic`, read the black box, forward it. (d) Optionally forward the `DebugPrintf` "PEEPOCRASH" lines it already receives.
2. **Room server (DO):** accept the forwarded report (or the `PKT_CRASH` opcode), stamp player + timestamp, log it.

Both are small. The black-box byte layout is the contract; once frozen, they can be implemented independently and in parallel with the ROM work.

## Assumptions to confirm with Yikes

- The deployed player build is **not** `RELEASE=1` (else `DebugPrintf` and `AGB_ASSERT` are stripped and we lose the self-report channel; the emulator harvest still works).
- The customized mGBA can read GBA RAM and forward bytes (near-certain for a custom emulator, but it is his code).
- The room server can take one new message/opcode.

## Open questions for Sol

- Black-box address discovery: magic-scan vs build-emitted symbol vs both. Recommendation: magic-scan primary.
- Ring-buffer depth N vs the 256-byte packet ceiling (tradeoff: more history vs one-shot send). Persistence path has no size limit; the packet does.
- Should the recursion guard *also* be submitted as a standalone fix PR first (it is a real crash fix independent of telemetry), or land as part of the crash-logging series?
- Is an in-ROM VBlank hang-watchdog worth the complexity if the emulator already watchdogs the heartbeat? (Leaning: no, keep the ROM simple, let the emulator own hang detection.)

## Build phases

1. ROM: black box + breadcrumb hooks + `magic` + heartbeat (no behavior change, pure observability). Build + verify.
2. ROM: guard-sensors, starting with the recursion guard (crash fix + first sensor).
3. Freeze the black-box byte layout; hand the two interface specs to the friend's side.
4. Optional reset-survival persistence + CLAUDE.md gate entry.
