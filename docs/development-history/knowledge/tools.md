# Tools — exact recipes (don't relearn these)

Paths assume repo root `/Users/aharonahdoot/Projects/GCDecomp`. The ISO is `Super Mario Strikers (USA).iso` in the repo root; export it as `STRIKERS_ISO`.

## Build
```sh
cd StrikersRecomp && cmake --build build -j8        # GUI build, Aurora ON  -> ./build/StrikersRecomp
cd StrikersRecomp && cmake --build build-headless   # headless, Aurora OFF (logic/timing tests)
```
Linker prints a harmless `ignoring duplicate libraries` warning — not an error. A standalone clang linter will flag `core/cpu.h not found` / unknown `CPUState` in `runtime/host/*.c`; those are false positives (no CMake include paths) — trust the cmake build result.

**FORK ERA (63/S11, 2026-07-03):** Aurora is an OWNED HARD FORK at `GXRuntime/graphics/aurora/`
(provenance in its `UPSTREAM.md`; baseline = upstream `0549581` + the retired patches 0001-0007).
Aurora changes are normal GXRuntime commits there. Reference `aurora/` is pristine and
read-only forever — never patch/edit it; builds no longer read it. Both build systems default
to the fork (`GXRUNTIME_AURORA_DIR` / `STRIKERSRECOMP_AURORA_DIR`); a CMake cache from before
S11 holds the old `../aurora` path — reconfigure with `-D...AURORA_DIR=` pointing at the fork
if a build tree predates it. **Silent-test-drop trap (hit at the S11 move):** `DOLGX_FE_TRACE_DIR`
is a CACHE PATH computed relative to the frontend source dir; after any dir move/rename a stale
cache makes every fixture `EXISTS` check fail and ctest quietly drops 14→5 tests while still
printing "100% passed". Purge with `cmake -U DOLGX_FE_TRACE_DIR -S <src> -B <build>` and always
check the ctest COUNT, not just the pass line. dolgx_replay/dff2dolt binaries now live under
`<build>/graphics/frontend/` (GUI tree: `StrikersRecomp/build/GXRuntime/graphics/frontend/`).

**Release assert hygiene (frontend tests, 2026-07-09):** `graphics/frontend/tests/*`
use `assert()` for side effects (`writer.open`, `write_fifo`, `flush`, …). Config
`Release`/`RelWithDebInfo` define `-DNDEBUG`, which elides those calls → fixtures
never written → empty `frames` → segfault at `format_digest_line` (fault addr
`0x8`). Pre-existing (reproduced at A1 `09d5ec2`); U1 was falsely blamed. Fix:
each frontend test `#undef NDEBUG` before `<cassert>`, plus CMake
`gxr_keep_asserts` (`SHELL:-UNDEBUG`) on those targets. Always run the two
crashers under **Release** once after any test-harness change:
`build-headless/graphics/frontend/{replay_digest_tests,aurora_recomp_frontend_tests}`.

*Historical (pre-63/S11), kept for archaeology:* patches lived in
`third_party/aurora-recomp/patches/` + `apply-patches.sh` and were applied to the standalone
`aurora/` working tree — uncommitted, lossy (`gx/recomp.hpp not found` = stack not applied; one
instrumentation change was lost that way: DOL_TEXTURE_CACHE_LOG, stripped in GXRuntime
f1d7261 — re-add in the fork if wanted). That mechanism is deleted; the fork commit history is
the durable record.

Exercise the GXRuntime-owned frontend against live WGPIPE without changing the
active renderer:
```sh
env DOL_AURORA_RECOMP_FRONTEND_SHADOW=1 \
  STRIKERS_ISO="$PWD/Super Mario Strikers (USA).iso" \
  StrikersRecomp/build/StrikersRecomp --max-blocks 6000000000
```
This shadow path feeds fragmented FIFO bytes into `RetailGxFrontend` and logs
`[aurora-recomp] shadow RetailGxFrontend rejected FIFO...` on first parser
rejection, then disables itself. Rendering still uses Aurora's old live path.
Use it to find frontend coverage gaps before sink cutover.

Focused draw-transform export (session 55): this turns on the same shadow
frontend automatically and prints bounded `ConsumedDraw` transform snapshots
without changing the live renderer:
```sh
env DOL_AURORA_RECOMP_DRAW_TRANSFORM_LOG=1 \
  DOL_AURORA_RECOMP_DRAW_TRANSFORM_MIN_FRAME=1200 \
  DOL_AURORA_RECOMP_DRAW_TRANSFORM_LIMIT=32 \
  STRIKERS_ISO="$PWD/Super Mario Strikers (USA).iso" \
  StrikersRecomp/build/StrikersRecomp
```
Optional filters: `DOL_AURORA_RECOMP_DRAW_TRANSFORM_FRAME=N` for one exact
present frame, `..._DRAW=N` for a frame-local draw index, `..._SEQUENCE=N` for a
packet sequence, and `..._LIMIT=N` for max logged draws. Output lines include
frame/draw/seq, primitive/fmt/count, current PN matrix, direct-payload PN matrix
mask, PN valid mask, viewport, projection, and each used PN matrix. Use
`MIN_FRAME` for interactive gameplay runs so FE/menu draws stay quiet until the
user has driven near the match.

Run the GXRuntime-owned direct Aurora replay after configuring the GUI build:
```sh
cmake --build StrikersRecomp/build --target gx_fifo_tests -j8
StrikersRecomp/build/aurora/tests/gx_fifo_tests \
  --gtest_filter=AuroraRecompReplayTest.RetailReplayResolvesAllRecompResources
```
The focused replay should pass 1/1. As of session 37, the unfiltered upstream
suite passes 173/177: the unrelated baseline failures are the patch-0003
1024-dimension expectation and three existing depth-peek tests. Do not
misattribute those four to indexed-XF replay changes.

In the non-login agent shell on this Mac, Homebrew CMake may not be on `PATH`. Use the stable `opt` symlink:
```sh
/opt/homebrew/opt/cmake/bin/cmake --build StrikersRecomp/build -j8
/opt/homebrew/opt/cmake/bin/ctest --test-dir GXRuntime/build-headless --output-on-failure
/opt/homebrew/opt/cmake/bin/ctest --test-dir DolRecomp/build --output-on-failure
```
Verified 2026-06-28: GXRuntime 1/1 and DolRecomp 8/8 pass. See `recomp-codegen.md`: those eight DolRecomp tests do not execute freshly emitted C against the Dolphin result corpus, so they do not clear the known emission defects.

DolRecomp emitted-code acceptance differential:
```sh
/opt/homebrew/opt/cmake/bin/cmake --build DolRecomp/build --target test_emitted_diff -j8
/opt/homebrew/opt/cmake/bin/ctest --test-dir DolRecomp/build -R emitted_diff --output-on-failure
DolRecomp/build/test_emitted_diff
```
Current expected result for the selected emitted-diff gate: `142 PASS, 9 XFAIL, 0 FAIL, 0 XPASS`. XFAIL is green only for catalogued emitter defects in `recomp-codegen.md`; XPASS means the upstream emitter changed and the catalogue/local fixer plan must be retired or updated.

devkitPPC/libogc producer DOL build:
```sh
make -C DolRecomp/tests/dolphin clean all
```
This Mac has devkitPro at `/opt/devkitpro`; `tests/dolphin/Makefile` now autodetects it. The resulting `test_opcodes.dol` runs under Dolphin, but `printf`/`kprintf` does not reliably surface to host stdout, so the next producer should report through USB-Gecko/TCP or a `dolphin-emu/hwtests`-style machine-readable socket channel.

## Run + observe
```sh
export STRIKERS_ISO="/Users/aharonahdoot/Projects/GCDecomp/Super Mario Strikers (USA).iso"
env STRIKERS_GFX_LOG=1 STRIKERS_AUTO_INPUT=1 ./build/StrikersRecomp --max-blocks 6000000000
```
Run GUI builds **harness-backgrounded** (not with `&`, which orphans/kills on wrapper exit). It does NOT self-exit at a fixed present count — runs sustain thousands of frames (verified 1700+) until the window is closed, `--max-blocks` is hit, or it crashes. (An older note claimed it self-exits ~present 314; that is stale.)

Useful env flags: `STRIKERS_GFX_LOG`, `STRIKERS_HLE_LOG`, `STRIKERS_AUTO_INPUT`, `STRIKERS_AUTO_SKIP_CARD_PROMPT`, `STRIKERS_STATE_LOG`, `STRIKERS_AUDIO_LOG`, `STRIKERS_MOVIE_CADENCE_LOG`, `STRIKERS_FRAME_BLOCKS=N` (VI retrace cadence override; default 350000), `STRIKERS_MAX_ARRAY_VERTS=N` (16..65536; per-indexed-array vertex cap fed to Aurora, current default 2048; `8192` restored gameplay geometry under the local span-aware Aurora path, see `aurora-runtime.md` storage-overflow/span trap), `STRIKERS_DUMP_CTX=0x<addr>` (OSThread state/queue), `STRIKERS_DUMP_GAME_STATE`, `STRIKERS_PC_HIST`, `STRIKERS_BACKCHAIN`, `--max-blocks N`, `--trace-every N`, `--mmio-log`.

`STRIKERS_GFX_LOG=1` also logs per-frame indexed-array advertised totals (`[gfx] frame array uploads: calls=.. bytes=.. peak=.. cap=8MB`) — the diagnostic that localized the captain-screen abort to Aurora's 8 MB storage buffer. With the local span-aware Aurora patch, this counter can overstate actual pushed storage because Aurora uploads only the draw-required indexed prefix.

## Catch an Aurora abort backtrace fast
GUI aborts (Aurora `std::abort`/`ByteBuffer::resize`) exit 134 and may print nothing useful. Catch the stack with lldb (auto-input drives toward the crashing scene):
```sh
env STRIKERS_AUTO_INPUT=1 STRIKERS_AUTO_SKIP_CARD_PROMPT=1 \
 lldb -b -o "breakpoint set -n abort" -o "run" -o "bt 30" -o "kill" -o "quit" \
  -- ./build/StrikersRecomp --max-blocks 8000000000
```
The native frames (`aurora::gfx::push` ← `push_gx_draw` ← … ← `notify_GXCopyDisp`) name the exact failing path.

Memory card: slot A defaults to `strikers-g4qe01-slot-a.dolcard` in the current directory. Use `--card <path>` or `STRIKERS_CARD=<path>` to relocate it; `--no-card` exercises the old no-card flow. `STRIKERS_CARD_LOG=1` is a focused trace of probe/mount/free/open/create/read/write/status plus queued/dispatched callbacks.

Do not judge framerate with `STRIKERS_HLE_LOG=1`, broad `STRIKERS_GFX_LOG=1`, or noisy Aurora warnings enabled; those can emit hundreds of thousands of lines and make an otherwise-good GUI run feel choppy. StrikersRecomp defaults Aurora to error/fatal logging only; set `STRIKERS_AURORA_LOG=1` when you specifically need Aurora info/warnings. Use a clean env first, then profile with `sample` if choppiness survives.

Frame pacing check: with the default `STRIKERS_FRAME_BLOCKS=350000`, `STRIKERS_GFX_LOG=1` should warm up to about 60 FPS (`frame=60 ... fps=60.x`). The old 2,000,000-block cadence measured about 11 FPS on this Mac. If a future scene livelocks or runs too fast, override `STRIKERS_FRAME_BLOCKS` for the experiment and record the value.

## Screenshot (macOS) — the #1 historical time-sink
**If `screencapture` returns a solid-black ~107 KB image while the window still enumerates, the DISPLAY IS ASLEEP.** Fix first, before any Spaces/Metal/tooling theory:
```sh
caffeinate -u -t 3      # wake the display / assert user activity
osascript -e 'tell application "System Events" to set frontmost of (first process whose name contains "Strikers") to true'
# get window id from winlist (a tiny Swift CGWindowListCopyWindowInfo helper), then:
screencapture -x -l<wid> out.png
```
The formerly documented `scratchpad/cap.sh` is not present in the current checkout. Use `.tools/dolphin/bin/winlist` to obtain the Strikers window id after `caffeinate -u`, then `screencapture -x -l<wid> out.png`. `winlist` lines look like `wid=33358 owner=[StrikersRecomp] name=[StrikersRecomp] 80,33 1280x838` — match on the `owner=[StrikersRecomp]` field and extract the `wid=` value (not the line start):
```sh
WID=$(.tools/dolphin/bin/winlist | grep "owner=\[StrikersRecomp\]" | head -1 | grep -oE 'wid=[0-9]+' | cut -d= -f2)
screencapture -x -l$WID out.png
``` **Dead ends — do not retry:** `CGWindowListCreateImage` (obsoleted macOS 15), a custom ScreenCaptureKit CLI (fails `CGS_REQUIRE_INIT` — no WindowServer session), full-display capture when the window is on another Space.

## Find a stuck/idle thread
`sample <pid> <secs> -file out.txt` → the native stack shows `func_<guestaddr>` frames = exact guest PC. A PC pinned in the scheduler idle loop = main thread asleep (not a game loop). Pair with `STRIKERS_DUMP_CTX=<threadaddr>` for OSThread state/wait-queue.

For `--max-blocks` smokes, a final PC in `SelectThread` is not automatically a hang: the watchdog may catch the idle thread between VI wakes. Check frame/input logs, or count `deliver_external` hits with lldb, before treating it as a regression.

## lldb — watch who writes a guest value (decisive for "wrong value" bugs)
Release build has **no DWARF types** (`CPUState` is unknown to lldb), so address by raw offset. `CPUState.ram` is at offset **0xD80 (3456)**; at `mmio_install` entry `$x0` = `CPUState*`:
```sh
lldb -b -o "breakpoint set -n mmio_install" -o "run" \
  -o "watchpoint set expression -w write -s 4 -- *(char**)(\$x0 + 3456) + (<guest_addr> - 0x80000000)" \
  -o "continue" -o "bt 8" -o "continue" -o "bt 8" -o "kill" -o "quit" \
  -- ./build/StrikersRecomp --max-blocks 6000000000
```
Run lldb backgrounded (`timeout` is absent on macOS; let the script self-terminate after N hits + `kill`). Also: `breakpoint set -n abort` + `bt` instantly catches Aurora `FATAL`/`std::abort` with a clean backtrace. `frame variable`/`p <local>` are useless on the optimized build — use registers (`$x0..$x28`, `$w0..`) at function entry.

## Diff a recompiled function vs the decomp
Use the `melee-objdiff` skill (objdiff-cli) when you suspect a specific function's codegen is wrong.

## Dolphin — the ground-truth oracle (INSTALLED, verified)
Real Dolphin runs the actual ISO, so it answers the two questions nothing else can: *what should the screen look like* and *what value does the game really compute at addr X*. Installed via `brew install --cask dolphin` (build 2603a, Metal backend, JITARM64 — verified booting `Super Mario Strikers (G4QE01)`).
- **Binary:** `/Applications/Dolphin.app/Contents/MacOS/Dolphin`. Real flags (this build): `-e/--exec <iso>`, `-b/--batch` (no GUI chrome; needs `-e`), `-s/--save_state <file>`, `-m/--movie <dtm>`, `-d/--debugger`, `-v/--video_backend Metal`, `-C/--config <Sys>.<Sec>.<Key>=<Val>`, `-u/--user <dir>`.
- **Synthetic DOLs:** `-b -e <probe.dol> -u <user-dir> -v Null` boots a producer DOL, but stdout is not a structured output channel. For opcode/conformance goldens, drive Dolphin as the execution oracle and have the DOL emit machine-readable results over USB-Gecko/TCP or a hwtests-style socket.
- **DolRecomp full-state oracle:** `make -C DolRecomp/tests/oracle verify` builds the devkitPPC producer DOL and captures strict full-state goldens with Dolphin Interpreter64 (`CPUCore=0`). `make -C DolRecomp/tests/oracle diff` emits the same cases through DolRecomp C and compares full `CPUState`; current result is `239 cases, 99 XFAIL, 0 unexpected, 0 XPASS`. `make -C DolRecomp/tests/oracle dedicated` covers 26 branch/system/cache/external-control cases that cannot use the fallthrough trampoline. `make -C DolRecomp/tests/oracle verify-jit` is optional and compares JITARM64 drift against Interpreter64; do not make JIT the authority.
- **Turnkey scripts (self-contained, in `.tools/dolphin/` — full docs in its `README.md`):**
  - `./dolphin_fe.sh` → boots and jumps straight to a **live Front-End menu** (loads save-state slot 1), screenshots, leaves Dolphin running to inspect. This is the ground-truth FE we compare against.
  - `./dolphin_run.sh out.png 22 "-d"` → boot + screenshot (`-d` = debugger).
  - `./dolphin_drive.sh` → (re)create the FE save state if `user/StateSaves/G4QE01.s01` is missing.
- **Read/compare a guest value (the matrix question):** launch with `-d`, then **View → Memory** (read `0x8032C060` view / `0x8032C0D0` modelview, big-endian floats), **View → Watch** (live list), or a **memory breakpoint** (who writes it). Compare to our recomp's value → *recomp bug vs genuine HW value*.
- **Driving input is non-obvious:** `osascript keystroke` is INVISIBLE to Dolphin (it polls HID state). Use `.tools/dolphin/bin/sendkey <macKeyCode>` which posts at `kCGHIDEventTap` (`7`=A,`36`=Start,`46`=save-state,`45`=load-state). Hotkeys bound in `user/Config/Hotkeys.ini` (M/N/P). The CLI `-s <state>` loads too early (lands at Health screen) → load via the N hotkey after boot instead.
- **The user is usually AT the machine during live sessions and has explicitly offered (2026-07-06) to perform finicky interactions on request** — navigating menus, playing into a match, reading the FPS/VPS/Speed overlay, confirming whether a cutscene/movie renders. ASK them (state exactly what to observe and what the possible readings mean) instead of building screenshot/sendkey automation loops; it is faster and they prefer it. Automation remains the fallback for unattended runs (attract demo match ≈ 2-3 min idle from title reaches live gameplay with zero input).
- **GX stream ground truth:** Tools → FIFO Player records a `.dff` of a frame's GX command stream — diff against what we feed Aurora.

**Dolphin Memory Engine (DME): intentionally NOT installed.** Everything it offers for us (live watch, memory breakpoints, value scan) is in Dolphin's built-in `-d` debugger, in-process and setup-free. DME needs the macOS code-sign dance (self-signed cert + `MacSetup.sh` re-signing Dolphin after every update) for zero new capability — and we already have exact addresses from the decomp symbol map, so we never need to scan. Skip it.

### THP movie comparison
The FE save-state helper and scripted key timing can skip/miss the intro movie; title-screen captures are not evidence about movie cadence. For this path, launch a normal Dolphin boot with `-d`, leave it running, and let the user drive:
```sh
/Applications/Dolphin.app/Contents/MacOS/Dolphin \
  -u "$PWD/.tools/dolphin/user" \
  -e "$PWD/Super Mario Strikers (USA).iso" -d -v Metal
```
Use Dolphin early to establish the genuine movie's cadence and transitions before tuning the recomp.

Clean recomp movie run:
```sh
env STRIKERS_ISO="$PWD/Super Mario Strikers (USA).iso" \
  STRIKERS_AUTO_SKIP_CARD_PROMPT=1 \
  StrikersRecomp/build/StrikersRecomp --max-blocks 6000000000
```
`STRIKERS_MOVIE_LOG=1` traces THPSimple/THPVideoDecode, control state, plane hashes, LC activity, and I8 texture loads. It is unsuitable for subjective pacing. Use it only for short correctness probes.

Focused movie probes:
- `STRIKERS_MOVIE_CADENCE_LOG=1` logs one-second buckets of `GXCopyDisp` presents, changes of `SimpleControl + 0x168` displayed texFrame, and THP audio ring state (`audio_dec`, `audio_out`, summed `audio_valid`, current `audio_out_valid`). After constant-time generated dispatch, the movie sustains 60-61 presents/sec and 30-31 texFrame changes/sec through at least 1,900 movie frames. The superseded linear dispatcher fell to 52-58 / 26-29 late in the movie.
- `STRIKERS_AUDIO_LOG=1` logs AI control writes and AID chunk source/peak. Use it to distinguish silence (peak 0 chunks) from a backend failure.
- `DOL_AUDIO_QUEUE_LOG=1` logs SDL queue/throttle state in the GXRuntime Aurora backend. `DOL_AUDIO_PREBUFFER_MS=<20..500>` and `DOL_AUDIO_MAX_QUEUE_MS=<prebuffer..1000>` tune the generic backend queue cushion; defaults are 40 ms prebuffer and 250 ms maximum queue. A healthy post-dispatch-fix movie stays near the cap and records throttle waits; the old broken path repeatedly reached 28-32 queued bytes.

Use a clean run plus `sample <pid> 5 -file <out>` for performance.

## After regenerating from DolRecomp
`python3 StrikersRecomp/tools/fix_generated.py` — corrects FP/PS1 emission and replaces the generated linear chunk dispatcher (idempotent). See `recomp-codegen.md`.

Strikers CMake intentionally fails configuration if `generated.h` lacks the `DolRecomp constant-time chunk dispatch` marker. Run the fixer; do not remove the guard. Any future fixer rule must also add a named defect/upstream-remedy entry to the ledger in `recomp-codegen.md`.

## Melee second-title CPU ABI smoke
`MeleeRecomp` is a read-only/non-git prototype. Compile its unchanged generated code and host scaffold against GXRuntime's CPU header/library without copying or editing it:
```sh
cd /Users/aharonahdoot/Projects/GCDecomp/MeleeRecomp
clang -std=c11 -O0 -I../GXRuntime/include -Iruntime -Igenerated \
  runtime/host/loader.c runtime/host/boot.c runtime/host/mmio.c \
  runtime/host/hle.c runtime/host/main.c generated/generated.c \
  generated/chunks/*.c ../GXRuntime/build-headless/libgxruntime.a -lm \
  -o /tmp/MeleeGXRuntime
/tmp/MeleeGXRuntime --max-blocks 5000000
```
Expected current boundary, identical to `./build/MeleeRecomp`: 24,523 blocks; system call at PC `0x00000C00`; LR `0x80344578`; r1 `0x804EEBB8`. This proves CPU ABI/core compatibility only, not loader/boot/device integration.

## Deterministic save-state differential harness (session 52)
The recomp executes **byte-deterministically**: two identical runs produce identical 24 MB MEM1
(verified `cmp memA memB` after `--max-blocks 60000000`). That makes snapshot/restore + a memory diff
a reliable, agent-drivable way to localize gameplay bugs WITHOUT manual menu navigation or a controller.

GXRuntime owns the save-state service (`GXRuntime/include/gxruntime/savestate.h` + `src/savestate.c`):
captures the CPUState register POD prefix (`offsetof(CPUState, external_read)`, excludes host pointers)
+ named memory regions (MEM1). Restore preserves host function pointers/`ram` (only the register prefix
is overwritten), so a freshly-`cpu_init`'d process resumes at the snapshot's pc.

Strikers CLI (`runtime/host/main.c`):
- `--snapshot-at-block N --snapshot-out PATH` — deterministic capture at block N (automated/agent).
- `--snapshot-out PATH` then `kill -USR1 <pid>` — interactive capture at any gameplay moment (no hotkey).
- `--restore PATH` — load the snapshot over the booted state and resume at its pc (deterministic; two
  restores produce identical MEM1, verified).
- `--dump-mem PATH` — raw guest-BE MEM1 at exit, directly diffable against a Dolphin RAM dump.

Analysis (`StrikersRecomp/tools/state_diff.py A.bin B.bin [--top N] [--json] [--gap N]`): coalesces
differing bytes into runs and labels each with the nearest preceding decomp symbol, so a diff reads e.g.
`0x8032C0D3 gx_modelview+0x3`, `g_FrameCounter`, `comUpdateTask` — pointing at the subsystem, not a raw
offset. Exit 0 = identical, 1 = differences. Symbols from `smstrikers-decomp/config/G4QE01/symbols.txt`.

Typical agent loop (recomp-vs-recomp, e.g. before/after a fix):
```
./build/StrikersRecomp --restore gameplay.dols --max-blocks 5000000 --dump-mem before.bin
# (apply fix, rebuild)
./build/StrikersRecomp --restore gameplay.dols --max-blocks 5000000 --dump-mem after.bin
python3 tools/state_diff.py before.bin after.bin --top 40
```
Capture a reusable gameplay snapshot ONCE (drive to gameplay, `kill -USR1`), then iterate forever from it.

**CONFIRMED LIMITATION (session 52): restore-RESUME hangs — device state must be captured for v2.**
A gameplay snapshot restores cleanly (pc/MEM1 load, `exception none`) but the game then **freezes**: a
restore+2M-block dump and a restore+20M-block dump are BYTE-IDENTICAL (MEM1 does not advance at all), and
`sample` shows the host loop spinning in `interrupt_poll`. The guest spin-waits on a device/MMIO condition
the RESET device state never satisfies. Root: the snapshot captures **MEM1 + CPU registers only**; device
state lives in `static` globals — `interrupts`/`si`/`vi_clock` (interrupt.c), `g_mmio_bus`/`g_exi`/`g_di`
(mmio.c), `g_audio_dma` (audio.c) — plus Strikers HLE callback/card state, none serialized. Aurora GPU
state (textures/arrays) is also host-side, so even a resumed sim renders black until the game re-issues GX
resources. **v2 fix:** add each device struct as a snapshot region (exclude/rebind the host function
pointers in `DolDi`/`DolExi`); optionally snapshot Aurora resource state for visual restore. **What v1
DOES give (enough for the differential):** a faithful capture of the recomp's gameplay **MEM1** — extract
it (`dd if=snap.dols of=mem1.bin bs=1 skip=3436`, = 16-byte header + 20-byte region header + 3400-byte CPU
POD prefix) and diff vs a Dolphin **Dump MRAM** (Memory debugger export menu, guest-BE) at the same static
moment. No resume needed for the structural diff.

### Single-function differential (`--call`) — the RIGOROUS, RESUME-FREE recomp-vs-Dolphin test (session 52)
Independent free-running emulations can NEVER be at the same dynamic moment (RNG/timing diverge → ~18 MB
of MEM1 noise), so a full-state diff of two separate runs is useless, and full-state RESTORE-resume hangs
(device/Aurora state, above). The rigorous escape is to compare ONE function on IDENTICAL inputs:
1. **Dolphin (oracle, the goalie animates CORRECTLY there).** Boot `-d`, into a match. Breakpoints → add
   the function entry addr (Instruction/Execute). Press **Play**; it re-pauses. **Verify PC == the func
   addr** (a wrong PC = it didn't hit). Read the EABI args (`r3`=this, `f1`/`f2…`=float args; raw FPR
   bits). **View→Memory→Dump MRAM** = `in.raw`. **Step OUT** (not Step Over — Step Over at a func entry
   steps one instruction; the in/out dumps will be byte-identical if you used Step Over). **Dump MRAM** =
   `out.raw`. (Caveat: "Step Out" can fold in a VI-interrupt's side effects — OS/thread/pad deltas — filter
   those regions.)
2. **Recomp.** `./build/StrikersRecomp --restore-mem in.raw --call 0xADDR --dump-mem r.bin --no-card
   --max-blocks 80000000` PLUS the args AND the EABI base registers the function needs:
   `--set-reg 1=<stack> --set-reg 2=0x8037CBE0 (SDA2 _SDA2_BASE_) --set-reg 13=0x80379040 (SDA _SDA_BASE_)
   --set-reg 3=<this> --set-fpr 1=0x<f1 raw f64 bits>`. **Forgetting r2/r13 makes the function read small-data
   globals from address 0 → garbage/fault.** `--call` runs headless (no Aurora), executes the function in
   isolation (no interrupts/device), and stops at an `lr` sentinel return.
   - **AMBIENT SPR STATE — MANDATORY for any paired-single function (session 53).** A bare `--call` skips
     boot/OSInit, so `HID2[PSE|LSQE]` is clear and the FIRST `psq_l`/`psq_st` (pervasive in SDK
     vector/matrix/physics math) raises a spurious `PROGRAM(ILLEGAL)` at **blocks=0**, looking exactly like
     a codegen fault. `--call` now **defaults `HID2=0xB0000000` (PSE|LSQE|LCE)** so the function runs; it
     prints `[call] ambient SPR default: hid2=...`. The GQR *contents* (SDK fastcast quant, e.g.
     `gqr[5]=0x00070007`, `gqr[6]=0x3D043D04`) are still 0 in bare mode → `psq` with `I=5/6` silently
     stores as float. For a FULLY faithful differential **add `--restore <gameplay .dols>`** before
     `--restore-mem`: it loads real HID2+GQR+MSR (restore runs after the default, so it wins), then
     `--restore-mem` overlays Dolphin's MEM1 and `--set-reg/--set-fpr` override the ABI args. Recipe used
     for the goalie: `--restore gameplay.dols --restore-mem goalie_in.raw --set-reg 1=.. 2=.. 3=.. 13=..
     --set-fpr 1=.. --call 0xADDR --dump-mem r.bin`.
3. **Diff:** `python3 tools/state_diff.py r.bin out.raw --ignore-uninit --named-only --top 40`. Divergent
   named symbols name the wrong field. IDENTICAL → the function is correct; move the breakpoint up/down the
   call tree. **A heap object (e.g. the actor at `0x816xxxxx`) is NOT near a symbol → `--named-only` HIDES
   it; check the object's own address range separately, and do the 3-way `in`/`r.bin`/`out.raw` byte check
   at each surviving run to tell "recomp wrote it wrong" from "Dolphin's Step-Out interrupt wrote it and our
   isolated call correctly did not."**
- **Find an object's pointer yourself** by scanning a Dolphin MRAM dump for its vtable address (e.g.
  `__vt__6Goalie = 0x802A6428`; matches at word-aligned offsets = object pointers) — removes the
  error-prone manual `r3` read. (Goalies were at `0x815f5c68`/`0x816126b8`.)
- **First result (session 52) — CORRECTED (session 53):** the recomp `Goalie::Update` (`0x8004E094`)
  "FAULTS (PROGRAM, blocks=0)" was **NOT a recomp divergence — it was the HID2 ambient-SPR gap above.** The
  fault was `psq_st` at `0x8004E0A4` hitting `psq_check_enabled` with `HID2=0`. With the ambient default (or
  `--restore gameplay.dols`), `Goalie::Update` runs **392 blocks, returns clean, exception none**, and its
  MEM1 output **matches Dolphin** except (a) OS/interrupt/timer/pad globals (`DefaultThread`,
  `__OSLastInterruptSrr0`, `m_DeltaT__11cPadManager`, `s_Next__9PadStatus`, `TypeTime`) that Dolphin's
  Step-Out VI-interrupt advanced, and (b) the `rm$1049` AI-decision scratch BSS — and the 3-way check proves
  our isolated call left `rm$1049` *untouched* (`in==r.bin`) while Dolphin *wrote* it (`in!=out.raw`), i.e.
  Dolphin's capture folded in non-Update code. **CONCLUSION: `Goalie::Update`'s computation is faithful;**
  the residual goalie T-pose is not in this function. Session 57 later traced the exact kickoff-state
  collapse to lazy FPU restoration across `OSLoadContext`, not skeleton/render code. Do NOT add a
  `fix_generated.py` rule for `Goalie::Update`. Lesson: always establish ambient SPR state before reading
  a blocks=0 paired-single "fault" as codegen.

**Stage 2: Dolphin import (run-1-frame differential, after v2 device capture).** Load Dolphin's gameplay
MEM1 + registers into a `.dols`, restore, run one frame, diff vs Dolphin's next frame. MEM1 imports
directly (both guest-BE); registers (pc/gpr/fpr/gqr) need a Dolphin savestate parse or debugger export.

### Deterministic Strikers route to gameplay (session 56)
Use the title-policy input driver when a renderer/runtime experiment needs a
repeatable live match:
```sh
env STRIKERS_MASH_TO_GAMEPLAY=1 \
  STRIKERS_ISO="$PWD/Super Mario Strikers (USA).iso" \
  StrikersRecomp/build/StrikersRecomp \
  --card StrikersRecomp/strikers-g4qe01-slot-a.dolcard
```
It reports controller 1 connected throughout and emits eight held/eight released
frames so every menu sees new `JustPressed` edges. Its state phases are:
- A + Start + Left until choose-side assigns player 1;
- A + Start after assignment, so later selectors do not keep moving left;
- A only in prematch state `0x100`, so the cutscene skips without pausing;
- neutral after the transition from prematch to gameplay state `0x2`.

`STRIKERS_STATE_LOG=1` adds route evidence. A successful run logs
`prematch reached; mashing A only` and then
`gameplay reached; controller is now neutral`; it must not subsequently log a
transition to pause state `0x1`. `STRIKERS_INPUT_SCRIPT` remains available for
arbitrary semicolon-separated
`start,end,buttons,stick_x,stick_y[,analog_a]` events, with end exclusive.

### LLDB watchpoint on a guest MEM1 address (session 57)
To catch the first writer to a corrupt guest value, break at any host notify
hook such as `notify_dWorldQuickStep`. On arm64, `x0` is the `CPUState*`; its
MEM1 host pointer is at `CPUState + 0xD80` in the current ABI. Convert a guest
cached address to a host address:
```
host_address = cpu->ram + (guest_address - 0x80000000)
```
Then install a raw hardware watchpoint:
```lldb
watchpoint set expression -w write -s 4 -- 0xHOST_ADDRESS
```
The current guest PC is at `CPUState + 0x280`. MEM1 bytes are guest
big-endian, so an LLDB condition that reads the watched word as a native
integer must byte-swap it. For a zero-initialized field, a zero-to-nonzero
condition is often enough and avoids encoding the target float. Session 57
used this recipe on the character body's force-Y word and stopped at guest
`0x8021F8A4`, the `dSetZero` store that first wrote the stale `f0` NaN.

## .dolt GX trace record + replay (program 63, M1)
Record (any StrikersRecomp GUI run; recording forces the shadow frontend ON):
```sh
env DOL_AURORA_RECOMP_TRACE_OUT=/path/out.dolt \
    DOL_AURORA_RECOMP_TRACE_FRAMES=60:120 \
    STRIKERS_ISO="/Users/aharonahdoot/Projects/GCDecomp/Super Mario Strikers (USA).iso" \
    ./build/StrikersRecomp --max-blocks 800000000
```
`_TRACE_FRAMES=A:B` is a 1-based present-frame window, inclusive (omit = all
frames). The file closes itself when the window ends and stderr prints
`[trace] wrote <path> frames=N records=M bytes=K` — no need to wait for the
run to exit. Frame N = everything after present N-1 up to & incl. present N.

Replay (headless, no Aurora/GPU/ISO; builds in GXRuntime build-headless):
```sh
GXRuntime/build-headless/graphics/frontend/dolgx_replay out.dolt \
    --against-stats            # gate vs the trace's own PRESENT_STATS
    --write-digest golden.txt  # (re)generate a golden digest
    --digest golden.txt        # exact line compare vs a golden
```
Digest line: `frame N draws D verts V topo T store S elems E fnv X state H`
(verts/topo/store = the push_verts/push_indices/push_storage byte extents,
same triple as the backend shadow-extent gate). against-stats rules: draws
exact + vert band (native-ours in [0, 4*draws+4]); fails only on >2
consecutive mismatch frames; storage/topology never gated (s51). ctest
fixture: `replay_digest_tests` in GXRuntime build-headless.

### FE menu fixture (M1 gate) — regen recipe
Fixture: `StrikersRecomp/tests/traces/fe_menu.dolt` + `fe_menu.digest`,
gated by ctest `dolgx_replay_fe_trace` (registers only when the fixture
exists). Window 1150:1210 = FE menu steady state under STRIKERS_AUTO_INPUT=1
(boot logo ends ~frame 190; menu regime from ~1050; map a new window with
STRIKERS_GFX_LOG=1 and the `[gfx] shadow-diff` lines). Regen:
```sh
cd StrikersRecomp && env STRIKERS_GFX_LOG=1 STRIKERS_AUTO_INPUT=1 \
    DOL_AURORA_RECOMP_TRACE_OUT=tests/traces/fe_menu.dolt \
    DOL_AURORA_RECOMP_TRACE_FRAMES=1150:1210 STRIKERS_ISO="$STRIKERS_ISO" \
    ./build/StrikersRecomp --max-blocks 6000000000
# kill after the [trace] wrote line, then:
GXRuntime/build-headless/graphics/frontend/dolgx_replay \
    tests/traces/fe_menu.dolt --against-stats --quiet \
    --write-digest tests/traces/fe_menu.digest
```
Recording is fully deterministic (two runs = MD5-identical traces), so a
digest change after a code change is real, not noise. `zdraws` in the digest
= zero-vertex retail draws (Aurora counts them, frontend no-ops them; gate
is draws+zdraws == stats draws).

### Gameplay fixture (M2 gate) — regen recipe
Fixture: `StrikersRecomp/tests/traces/gameplay_kickoff.dolt` + `.digest`,
ctest `dolgx_replay_gameplay_trace`. Mid-match window via the mash route
(frame 3000 ≈ 95s; window kept short — gameplay churns ~1.9MB/frame of
MEM_UPDATE even with dedup; 61 frames = 117MB, 16 frames = 33MB). Regen:
```sh
cd StrikersRecomp && env STRIKERS_GFX_LOG=1 STRIKERS_MASH_TO_GAMEPLAY=1 \
    DOL_AURORA_RECOMP_TRACE_OUT=tests/traces/gameplay_kickoff.dolt \
    DOL_AURORA_RECOMP_TRACE_FRAMES=3000:3015 STRIKERS_ISO="$STRIKERS_ISO" \
    ./build/StrikersRecomp --card strikers-g4qe01-slot-a.dolcard \
    --max-blocks 6000000000
# kill after the [trace] wrote line (~100s; macOS has no `timeout` — use
# `& pid=$!; (sleep 210; kill $pid) & wait $pid`), then:
GXRuntime/build-headless/graphics/frontend/dolgx_replay \
    tests/traces/gameplay_kickoff.dolt --against-stats --quiet \
    --write-digest tests/traces/gameplay_kickoff.digest
```
against-stats gates ONE PRESENT BEHIND (AuroraStats publish late — see
aurora-runtime.md "AuroraStats are published ONE PRESENT LATE"): an N-frame
trace gates N-1 pairs; the live `[gfx] shadow-diff` line prints `fe_zdraws`
and with the 63/S7 gate `mismatch_frames` stays 0 from boot through
mid-match (draws+zdraws==stats at lag-1).

## Mode B pixel replay (63/S8, M3)
The Aurora-linked `dolgx_replay` lives in the StrikersRecomp GUI tree
(build-headless prints "no --pixels support"):
```sh
cd StrikersRecomp && ./build/GXRuntime/graphics/frontend/dolgx_replay \
    tests/traces/boot_menus.dolt --pixels --quiet \
    --pixel-digest tests/traces/boot_menus.pixels   # gate vs golden
  # --write-pixel-digest <path>  (re)generate the golden
  # --png-dir <dir> --png-every 100  reference PNGs (12MB each stored;
  #   1210 frames ungated would be ~15GB — always pass --png-every)
```
Facts (full mechanism: aurora-runtime.md "Mode B pixel replay"):
- Needs a FROM-BOOT trace (`DOL_AURORA_RECOMP_TRACE_FRAMES=1:B`); windowed
  traces render all-clear or crash shader gen (boot-time state absent).
  boot_menus.dolt = mash route 1:1210 (26MB), also a ctest Mode A fixture
  (`dolgx_replay_boot_trace`).
- Deterministic to EXACT hash equality (warm and cold pipeline caches) via
  forced AURORA_SYNC_PIPELINES; a GPU window opens (works in agent shells);
  hashes are per-display-scale (1024×768 window → 2048×1536 retina).
- If a pixel run SIGSEGVs in build_shader_source: delete BOTH
  `~/Library/Application Support/dolgx_replay` and
  `~/Library/Caches/dolgx_replay` (poisoned persisted pipeline cache).
- Pixel gate is a MANUAL/local gate by design (GPU runs stay out of ctest);
  Mode A digests carry automation.

## Dolphin savestate as a scripted value oracle (2026-07-02, s63/S6 — the working path where gdb failed)
When you need Dolphin-side runtime VALUES (XF/light state, any RAM/host-video state) without GUI
driving: reach the scene, press hotkey **M** (Save State Slot 1), parse
`user/StateSaves/G4QE01.s01` with **`.tools/dolphin/bin/parse_state_lights.py`** (self-contained,
no pip deps; adapt its pattern-scan for other state). Format facts (Dolphin 2603a,
`Source/Core/Core/State.h`): 24B legacy header (game_id[6], pad, lzo_size==0, pad, time f64) →
u32 version_cookie + u32 len + version string → 16B extended header (u16 hver, u16 compression
1=LZ4, u32 payload_offset FROM END of this header, u64 uncompressed_size) → repeated
`[i32 compressed_len][LZ4 block]`. Guest MEM1 appears big-endian inside; Dolphin host-side video
state (xfmem etc.) is little-endian — scan for both endiannesses of your signature to tell the
game's RAM copy from Dolphin's live GPU state (s6 used cosatt/distatt (1,0,0)(1,0,0) f32 pairs
to locate light objects). HYGIENE: slot 1 (`G4QE01.s01`) is the maintained FE state — back it up
and restore after. **In-match states are build-specific:** `G4QE01-inmatch-s6.sav` is a
**chassis/recomp-core** savestate — loading it in stock `/Applications/Dolphin.app` yields black
(2026-07-09 A4). Stock visual A/B uses `G4QE01-inmatch-stock.s01` (user-saved mid-match on stock
Dolphin; also may appear under `~/Library/Application Support/Dolphin/StateSaves/` if Dolphin was
started without `-u .tools/dolphin/user`). Do not mix chassis and stock states.
- Dolphin menu drive from the FE state, blind-verified by screenshots: N (load slot 1) → A
  (dismiss "choose a side") → Left ×2 (join) → A ×3 (team/sidekick/stadium defaults) → ~15s load
  → A ×3 (skip prematch) = in-match Mario vs Luigi, same defaults as our
  STRIKERS_MASH_TO_GAMEPLAY route.
- **GDB stub (`-C Dolphin.General.GDBPort=N` + devkitPro powerpc-eabi-gdb) — know the limits:**
  attaches fine at boot (CPU waits paused), but (a) async break-in times out and a piped gdb
  auto-answers "Stop debugging? Y" and disconnects, (b) the stub does NOT re-listen after
  disconnect (restart Dolphin to reattach), (c) a `break *addr` set at the boot stub never fired
  under JITARM64 in-game (suspect insertion-vs-DOL-load or JIT/bp interaction). Drive it with a
  FIFO stdin (`mkfifo`; keep a `sleep`-holder writer so echoes don't EOF gdb). Prefer the
  savestate parse for value extraction; keep gdb for experiments where you can set state while
  paused pre-boot.

## Dolphin .dff oracle pipeline (63/S9, M4): record → convert → replay → compare
The renderer-layer diff with zero recomp-CPU influence: the SAME bytes replay
through Dolphin and through us.
1. **Record** (manual, Dolphin GUI): Tools → FIFO Player → Record → save
   `scene.dff` (FifoDataFile v6; header carries BP/CP/XF/XFRegs/TMEM snapshot
   at record start + per-frame raw FIFO bytes + fifoPosition-sorted memory
   updates with PHYSICAL addresses).
2. **Convert**: `GXRuntime/build-headless/graphics/frontend/dff2dolt
   in.dff out.dolt` — synthesizes the FifoPlayer::LoadRegisters state-restore
   preamble as raw FIFO commands (BP skip {45,47,48,52,63,65,67}; XF skip
   {1007,1013-17,1027-3E,1048-4F}); TMEM snapshot + ClearEfb NOT restored
   (v1 gaps, stats-reported; our 3 scenes carry ≤510 nonzero TMEM bytes).
   Converted traces have NO PRESENT_STATS — replay closes frames at
   FRAME_BEGIN/EOF instead (both Mode A and pixels; backend traces unchanged).
3. **Golden PNGs**: `caffeinate -dims -t 90 & /Applications/Dolphin.app/Contents/MacOS/Dolphin
   -b -e scene.dff -u <FRESH-userdir> -v Metal -C Dolphin.Movie.DumpFrames=True
   -C Dolphin.Movie.DumpFramesSilent=True -C Graphics.Settings.DumpFramesAsImages=True`
   → `<user>/Dump/Frames/framedump_N.png`, 640x480, MD5-deterministic across
   runs. **Master switch is Dolphin.Movie.DumpFrames** (the Graphics key only
   selects PNG-vs-ffmpeg). Playback LOOPS regardless of
   Dolphin.FifoPlayer.LoopReplay=False — kill after dumps appear (~25s).
   The first ~2 presents are not dumped (start_menu 3fr→1 png, fe 8→6,
   gameplay 22→20; use pixel_diff --auto-align). GOTCHAS: kill zombie
   Dolphins first (single-instance lock = silent instant exit, zero-byte
   log); `.tools/dolphin/user` makes batch .dff playback exit instantly
   (unexplained) — use a fresh scratch user dir; zsh failed-glob (`rm dir/*`)
   aborts the whole compound command before Dolphin even launches.
 4. **Pixel compare (DR-15 ranking — prefer F1→F2→F3; host screencapture LAST):**
    ```
    # ACCURACY GATE (F3): F2 EFB 640x480 vs Dolphin DumpFrames — no bilinear scale
    python3 GXRuntime/graphics/frontend/tools/pixel_diff.py \
      --efb-native --ours <efb-png-dir> --golden <Dump/Frames> [--diff-out d]
    # pair by filename index (efb_NNNN ↔ framedump_N); default when names match
    # --require-efb-native  → exit 2 if mode would fall back to host-resize

    # TRIAGE ONLY (demoted old A4 path): host window / retina capture
    python3 .../pixel_diff.py --host-resize --ours <window-pngs> --golden <Dump/Frames> \
      [--auto-align]   # auto-align is triage; prefer --pair-by-index for gates
    ```
    Auto mode: equal first-frame sizes → EFB-native; else host-resize. Stderr
    banners: `*** EFB-native mode ***` vs `*** host-resize triage mode ***`.
    A 448-line "XFB letterbox" crop is WRONG (costs 0.2 band — bottom dark is
    game overscan). Metric floor on matched content ~1-2 LSB.
    **Dolphin golden recipe (scripted):**
    `GXRuntime/graphics/frontend/tools/dolphin_efb_golden.sh -e scene.dff -o goldens/`
    (`--dry-run` prints the exact DumpFrames command; needs Dolphin app + .dff).
    ctest: `pixel_diff_efb_native` (synthetic fixtures, no GPU).
 5. **Histogram**: `dolgx_replay <trace> --histogram --quiet` — BP/CP/XF reg,
    genMode TEV/texgen/indirect, tex/TLUT/copy format, draw-prim tallies from
    the frontend event observer. Run over the corpus for the M6 module queue.

## gxpo accuracy oracle (Program 68 — product path, 2026-07-10)

**G-GX accuracy** = `gxpo G0–G5 green on the declared corpus` (not F1/F2/F3).
Full product surface: `GXRuntime/docs/gxpo.md`. Spec:
`runtimeharness/.fablize/handoffs/68_gx-parity-oracle/03_GPNF_SPEC.md`.

```sh
# Dry-run / CI synthetic (no ISO, no oracle live required for dry-run):
bash GXRuntime/graphics/frontend/tools/gxpo_run.sh --dff flat_tri --dry-run --ci
bash GXRuntime/graphics/frontend/tools/gxpo_run.sh --corpus --dry-run --ci

# Live dual-side (needs dolphin-oracle @ 1ccbcaa + build-headless-b):
export GXPO_LANE=impl-b
bash GXRuntime/graphics/frontend/tools/gxpo_run.sh --dff flat_tri \
  --out /tmp/p68_impl-b_run
python3 GXRuntime/graphics/frontend/tools/gxparity.py \
  /tmp/p68_impl-b_run/oracle /tmp/p68_impl-b_run/gx --out /tmp/p68_impl-b_run/compare

# Certificate contract + private registry:
python3 GXRuntime/graphics/frontend/tools/gxpo_registry.py skeleton
python3 GXRuntime/graphics/frontend/tools/gxpo_registry.py --root GXRuntime validate
# Retail bytes: git-ignored goldens/private/; hashes only in REGISTRY.json
```

Gates: G0 config → G1 input → G2 state → G3 vertex → G4 EFB → G5 copy → G6 present(info).
Exit 0/2/10/20/30/40/50. Claim: `parity-with-Dolphin-SW@1ccbcaa`.
`dolphin-oracle` flock under `runtimeharness/.fablize/locks/` (one Dolphin at a time).

**QUARANTINED (DR-O10 — never extend/import as gates):** `gp_state_diff.py`,
`pixel_diff.py`, `graphics_lockstep.sh`, `dolphin_efb_golden.sh`.

## DR-15 visual oracle ranking (WS-F F1–F3 — QUARANTINED 2026-07-09)

Superseded by **gxpo** above. Historical triage ranking only:

| Rank | Artifact | Tool | Gate? |
|---|---|---|---|
| 1 | F1 GP state (one-sided) | `gp_state_diff.py` | **quarantined** |
| 2 | F2 fixed EFB PNG | `dolgx_replay --efb-png-dir` | triage surface |
| 3 | F3 pixel DumpFrames | `pixel_diff.py` + `dolphin_efb_golden.sh` | **quarantined** |
| last | Host screencapture banded SSIM | `pixel_diff.py --host-resize` | **triage only** |

Silent wrong pixels with A1 completeness green is still a fail — prove it with gxpo.

## dolgx_replay --core (63/S12, Mode B2 gxcore replay)

Same shape as `--pixels` but draws route through the gxcore plan pipeline
(local RetailGxFrontend → GxCoreSink → fork gxcore_draw); the live aurora gx
layer never sees the stream, and no GXInit baseline is fed (dff-converted and
from-boot traces are self-contained by construction).

    ./build/GXRuntime/graphics/frontend/dff2dolt tests/traces/dff/start_menu.dff /tmp/s.dolt
    ./build/GXRuntime/graphics/frontend/dolgx_replay /tmp/s.dolt --core \
      --write-pixel-digest run1.pixels --png-dir pngs1     # generate
    ... --core --pixel-digest run1.pixels --quiet          # gate (determinism)

Gap counters print at exit (`gxcore GAP <name>=<n>`) — they are the S13-S16
demand signals; `submitted`/`rejected` must match the scene's draw count.
Works in the agent shell (window opens, EFB readback path identical to
--pixels). GUI tree only (DOLGX_REPLAY_HAS_CORE).

**Dolphin golden regen note:** the batch .dff dump recipe LOOPS the scene —
a 3-frame scene yields ~1000 framedumps in 25s. For a static scene (start
menu) `framedump_1.png` is the golden; delete the rest. S12 baseline
(start_menu, tol=8): gxcore band .2963 / ssim .3556 vs live-Aurora raw
.512/.545.

### F2 fixed-res EFB PNG dump (`--efb-png-dir`, DR-15 / WS-F)

Metric surface for RecompCore-grade accuracy. **Not** macOS screencapture and
**not** the host swapchain / retina backbuffer.

Mechanism: `aurora_set_efb_fixed_size(W,H)` pins `g_frameBuffer` /
`present_source()` to fixed WxH (default **640×480**), independent of the
1024×768 window. Readback is the existing Dawn `efb_readback` path
(`CopyTextureToBuffer` of present_source → packed RGBA8). When the flag is
off, present/XFB is unchanged.

```
# Preferred (Mode B2 / --core). GUI/Aurora tree; private build-headless-b ok.
./build-headless-b/graphics/frontend/dolgx_replay /tmp/s.dolt --core \
  --efb-png-dir /tmp/p66_b_efb --quiet
# → /tmp/p66_b_efb/efb_0001.png … (4-digit frame index; 640×480 RGBA8)

# Env alias (merge-safe with F1 CLI on dolgx_replay_core):
DOL_EFB_PNG_DIR=/tmp/p66_b_efb \
  ./…/dolgx_replay /tmp/s.dolt --core --quiet

# Size override:
  --efb-size 320x240          # or DOL_EFB_SIZE=320x240

# Validate count + IHDR size (no GPU; ctest-friendly):
python3 GXRuntime/graphics/frontend/tools/check_efb_pngs.py /tmp/p66_b_efb \
  --expect-count 3 --size 640x480

# F2 depth plane (shadows/z paths — DR-15 residual closed):
  --efb-depth-dir /tmp/p66_b_depth   # or DOL_EFB_DEPTH_DIR=
# → efb_z_XXXX.r32  float32 LE, z24/16777215, same WxH (640×480 → 1228800 B)
python3 GXRuntime/graphics/frontend/tools/check_efb_depth.py /tmp/p66_b_depth \
  --expect-count 3 --size 640x480
# If depth_peek unavailable: tool logs BLOCKED and writes NO zero goldens;
# check_efb_depth.py --allow-empty documents that residual.

# F3 consume: pixel_diff.py --efb-native --ours <efb-dir> --golden <Dump/Frames>
```

**Depth format:** `efb_z_XXXX.r32` = packed host LE float32 plane, one float per
pixel row-major top-left, value = `(z24 & 0xFFFFFF) / 16777215.f`. Source is
`depth_peek` full-frame snapshot of `g_depthBuffer` (not color-only present_source).
**Host-present triage:** `--png-dir` still writes `frame_XXXXX.png` at whatever
the window/EFB is; demoted to triage (DR-15). Prefer F2 for gates.
**--core note (2026-07-09):** gxcore `tex_copy_conv` can SIGSEGV on some dff
traces after U9 pixel-center WGSL; `--pixels` + F2 is the verified dump path
on start_menu until that is fixed. WGSL `clip.xy *=` was expanded (Dawn rejects
compound-assign on swizzles).

### F1 GP state lockstep dump (`--gp-state-dump`, DR-15 / WS-F)

Per-draw (and copy/clear) JSONL of frontend+gxcore-consumed state after the
plan is built — schema_version 1. Fields: frame/draw index, event_kind,
vertex/index counts, chanctrl + lights_mask + key light0, pos/nrm matrix FNV,
texgens + dualtex postmtx, TEV stage compact keys, PE/zmode/blend/cull,
bound tex identities (addr/fmt/wh/tlut), texmode0/1 samplers, efb_copy block
on copy events. **Not** host-present metrics.

```
# GUI tree (Aurora + --core):
./build/GXRuntime/graphics/frontend/dolgx_replay /tmp/s.dolt --core \
  --gp-state-dump /tmp/p66_a_state.jsonl --quiet
# env alias (same path):
DOL_GP_STATE_DUMP=/tmp/p66_a_state.jsonl \
  ./build/GXRuntime/graphics/frontend/dolgx_replay /tmp/s.dolt --core --quiet

# Diff two dumps (exit 0 equal; else first path + values, exit 1):
python3 GXRuntime/graphics/frontend/tools/gp_state_diff.py a.jsonl b.jsonl
python3 GXRuntime/graphics/frontend/tools/gp_state_diff.py --self-test

# Headless ctest (no GPU): gxcore_gp_state_dump_tests + gp_state_diff_tool_tests
# under GXRuntime/build-headless-a (or any BUILD_TESTING tree).
```

Serializer lives in `GXRuntime/graphics/gxcore/{include,src}/gp_state_dump.*`
(read-only of DrawPlan/EfbCopyCommand). Wire is in `dolgx_replay_core.cpp`
plan/copy observers. Rank failures by first state diverge **before** EFB/pixel
diff (see DR-15 table above).

## Headless frontend probe (link 3 libs) — 63/S16 pattern

To answer "what does the game's packet stream contain" WITHOUT the GUI/GPU (e.g. which draws bind
EFB-copy dests, what fog config, per-draw texture identity), write a tiny standalone that drives the
RetailGxFrontend with a custom `ar::AuroraRenderSink` watching raw packets, and link against the
headless static libs:
```
clang++ -std=c++20 -O1 \
  -IGXRuntime/graphics/frontend/include -IGXRuntime/include \
  probe.cpp \
  GXRuntime/build-headless/graphics/frontend/libgxruntime_retail_gx_frontend.a \
  GXRuntime/build-headless/graphics/frontend/libgxruntime_aurora_render_sink.a \
  GXRuntime/build-headless/graphics/frontend/libgxruntime_trace_io.a \
  GXRuntime/build-headless/libgxruntime.a -o probe
```
Feed it a `.dolt` (convert a `.dff` first: `graphics/frontend/dff2dolt recordings/gameplay.dff g.dolt`).
The record loop (GxWrite→write_fifo/flush, CallDisplayList→write_display_list, SetArray→set_cp_array,
MemUpdate→memcpy into a mem1 vector via `dol_gx_recomp_guest_to_physical`) is copyable from
`tools/dolgx_replay_core.cpp`. A mem1 resolver (init_callback) is needed so textures resolve.
Worked examples: `runtimeharness/.fablize/scratch/63_s16/efb_probe.cpp` (EFB-copy dest ranges vs
per-draw bound-texture address) + `fog_probe.cpp` (fog fsel/proj/range distribution).

## gxcore↔Dolphin register-coverage diff (parity audit)

To measure whether gxcore covers a Dolphin renderer feature, DIFF the authoritative register map against
what gxcore decodes — don't rely on the GapCounter stub list (it only sees instrumented features, so it
MISSES gaps; the BP+XF diff is what caught dual-tex/zfreeze/flat/EFB-format on 2026-07-05).
```
# Dolphin's map:
grep -oE "BPMEM_[A-Z0-9_]+ = 0x[0-9a-fA-F]+" dolphin/Source/Core/VideoCommon/BPMemory.h
grep -niE "XFMEM_[A-Z]+ = 0x1[0-9a-f]+" dolphin/Source/Core/VideoCommon/XFMemory.h   # + struct @ ~L440
# gxcore's decode sites (anything in Dolphin's map with 0 hits here = a gap):
grep -oE "bp_regs_\[0x[0-9A-Fa-f]+|bp_valid_\[0x[0-9A-Fa-f]+|bp\(0x[0-9A-Fa-f]+" GXRuntime/graphics/gxcore/src/gxcore.cpp
grep -oE "0x10[0-9A-Fa-f]{2}" GXRuntime/graphics/gxcore/src/gxcore.cpp        # XF regs
```
Feature-not-a-register surface = shader-gen uid_data: `PixelShaderGen.h` / `VertexShaderGen`(uid_data) /
`GeometryShaderGen` — map every field to a tracked gap; unmapped = miss. **Bit-exact oracle = the Dolphin
SOFTWARE backend** (`VideoBackends/Software/{Tev,TransformUnit,Rasterizer,Clipper,SetupUnit,SWEfbInterface}
.cpp`) — simplest per-feature reference; diff gxcore WGSL/plan math against it. RESUMABLE audit ledger +
full coverage matrix + exact next file: `handoffs/64_full-compat-program/PARITY_AUDIT.md`.

## Publication layout + sanitation recipes (Program 67, 2026-07-08)
Public repos: github.com/aharonahdoot/{RecompCore,GXRuntime,StrikersRecomp,DolRecomp}. **Work lands on
`public-main` locally** (tracked to each `origin main`); old private history stays on local `main` +
`backup-pre-publish` + the `*-archive` private repos. Remotes: satellites `origin`=new public repo,
`archive`=old private; dolphin-chassis `origin`=RecompCore, `upstream`=dolphin-emu/dolphin,
`upstream-local`=../dolphin; DolRecomp `origin`=ExpansionPak (pull-only), `fork`=aharonahdoot/DolRecomp
(fork main = verified series on the 2026-06-24 base; `pr-oracle-suite` = suite rebased on upstream tip →
PR #9; series 2–5 conflict with upstream's MFSPR/MTSPR rework — reshape per issue #8 feedback).
Sanitation scan set (run on tree AND history before anything goes public): `git log --all -i
--grep=claude`; `--format=%b | grep -ci co-authored`; `--format='%an <%ae>' | sort -u`; jargon grep
`"Program 6|arc [0-9]|G01[0-9]|fablize|GCDecomp|Users/aharonahdoot|Strikers (USA)|63/S"`; `find -size
+1M`. Traps that actually bit: `git add -A` RE-STAGES files you just `git rm --cached`-ed (verify with
`git ls-tree -r HEAD` after amend, not with status); case-insensitive APFS makes README.md silently
overwrite upstream's Readme.md; zsh does NOT word-split `$var` in `for`/`set --` loops; stale committed
test binaries make `make` skip regeneration (untrack + .gitignore, then rm local copies to force). Public
CI counts must be measured on a CLEAN CLONE (GXRuntime = 7 there, 16 with local trace fixtures). User
rules for public artifacts: OS-agnostic wording everywhere; upstream comms in plain first-person human
voice (no AI-polite framing).

## Certificate/demand tooling (Program 66 C-lane, 2026-07-09)
Two game-agnostic static analyzers in `StrikersRecomp/tools/` (inputs = symbols.txt-or-signatures +
generated chunks + main.dol [+ optional REL dir]; NO Dolphin build, NO ISO):
- **`sdk_register_map.py`** (C1): parses the dtk SDK C tree (default `smstrikers-decomp/src/Dolphin`)
  -> per SDK function the BP/CP/XF/MMIO registers it can write. Mechanisms: BP addr lives in bits
  24-31 of the RAS-written value; resolved via (a) in-function `SET_REG_FIELD(_,X,8,24,LIT)` and
  (b) `__GXData->FIELD` mirrors whose addr byte is fixed in GXInit (parsed field->addr map, incl.
  array families `BASE + i*K` expanded over the ENCLOSING `for(i<N)` bound — DON'T blind-expand to
  16 or suTs0/1 bleed into ZMODE/cmode0). XF = `GX_WRITE_XF_REG*` literal addr + `GX_WRITE_MTX_ELEM`
  (matrix mem). CP = `GX_WRITE_SOME_REG*(8,0xNN,..)`. MMIO = `__{VI,AI,DSP,PI,SI,EXI,DI}Regs[idx]=`.
  Output `sdk_register_map.json` (functions + register_index) pairs with A1's registry.cpp:
  `worklist = certificate-demand -> register-level demand \ capability`. G4QE01: 105 fns, 161 regs.
- **`demand_certificate.py`** (C3 v2): the per-game certificate. KEY TRAP: the recomp labels EVERY
  instruction (`label_XXXX:` per line), so labels are NOT basic-block boundaries — reset the
  abstract-register state only on FUNCTION change, else adjacent lis+addi never combine and split-
  const reassembly finds nothing. Emission forms: lis=`gpr[N]=((u32)(s32)(IMM)<<16)`,
  addi=`gpr[d]=gpr[s]+(u32)(s32)(IMM)`, ori=`gpr[d]=gpr[s]|0xNNNu`, full=`gpr[N]=0xNNNNNNNNu`.
  Lanes: split-const address-taken closure, direct-MMIO 0xCC00xxxx pokes (0xCC008000=GX-FIFO/WPAR),
  const-arg mining (arg regs gpr3-10 live at call site), `--closure` conservative mode, SMC ingest
  (`generated_smc.txt`), REL header+import parse, THP/audio-lib identity. Reassembly lifted G4QE01
  reachability 7281->8377 (unreachable 1309->213) vs v1. Idempotent; re-run prints delta vs on-disk.

- **`demand_certificate.py` is game-parameterized (C5, 2026-07-09):** `--game/--symbols/--chunks/
  --dol/--smc/--rel-dir/--out/--no-symbols`. No args = historical G4QE01. **No-symbols mode** (for a
  title with no decomp symbol map, e.g. Melee GALE01) rebuilds the function table from the recomp's
  OWN output: entries = call targets (`ctx->lr=…` then `ctx->pc=CONST`) ∪ addr-taken DOL data words
  in text ∪ DOL entry(0xE0). Needs zero game data. Recovered 21478 fns for Melee vs symbols' 8590
  for Strikers. Degrades to STRUCTURAL + HARDWARE-BLOCK demand tiers only (no SDK-lane names without
  signatures) — the conservative fail-safe surface. `--no-symbols` auto-enables when the symbols path
  is absent.
- **`corpus_report.py` (C5) = cross-game roll-up:** runs build_certificate over an extensible TITLES
  list → `compat_db.json` + `StrikersRecomp/docs/COMPATIBILITY.md` (D4). Two tiers: hardware-block
  heat map (every title, symbol-optional — "implementing {block} unlocks N%") and GX register demand
  (symbol titles only: demanded GX fns × C1 `sdk_register_map.json` → BP/XF/CP registers; pair vs
  A1 `registry.cpp` for the register-granular worklist). Add a title = one TITLES entry, no code.
  NB: repo-root `docs/` is NOT a git repo — COMPATIBILITY.md lives in `StrikersRecomp/docs/`.

- **C2 structured gap report (`gxruntime/gap_report.h`, 2026-07-09):** one JSON tripwire report per
  run, schema `{lane,kind,key,count,first_pc,notes,class}`, aggregated by (lane,kind,key). Call
  `gap_report_note(lane,kind,key,pc,notes)` at any silent-failure branch; `gap_report_add(...,n)` for
  aggregate counters (e.g. gxcore GapCounters where the value IS the fire-count). `--gap-report <path>`
  is wired into `dolgx_replay` (Mode A frontend rejections; `--core` gxcore counters) and StrikersRecomp
  `main` (mmio unknown-r/w bucketed by block, exi unclaimed-device, dsp unclaimed-mailbox, hle
  dispatch-miss). A clean run writes `[]`; `--core` on boot_menus surfaces the real `tlut_texture` S13
  gap. Diff the report against `certificate_*.json` for completeness proof. gxcore counters are consumed
  READ-ONLY from `sink.counters()` (never edit gxcore to report).
  - **Empty-report acceptance filter (C2, 2026-07-09):** every JSON row carries `"class"`:
    `gap` (counts) | `policy` (kind or notes contain `policy/`) | `diagnostic` (documented non-defect
    GapCounter). API: `gap_report_classify`, `gap_report_acceptance_size/total`,
    `gap_report_write_json_ex(path, acceptance_only)`. Documented diagnostic kinds (expand only with
    KNOWLEDGE cite): `gxcore/normals_ignored` (`aurora-runtime.md` — do NOT chase to 0). Full write
    never drops rows; acceptance counters/filter exclude policy+diagnostic. Host/replay print
    `acceptance N` beside raw distinct.
- **Baselining a segfault in a shared/dirty tree (non-destructive):** when another lane has uncommitted
  work in the same repo, DO NOT `git stash` (clobbers them). Instead `git worktree add --detach <tmp>
  <HEAD-or-suspect-sha>`, build+run the failing test there, then `git worktree remove --force`. This
  proved `aurora_recomp_frontend_tests`/`replay_digest_tests` segfaults were pre-existing at impl-a's
  U1 commit 3dbb932, not from C2 — decisive attribution without touching the working tree.
