# PROGRAM 65 (Dolphin chassis) — per-arc resume prompt (v2, rewritten after the arc-2 retro)

> **STATUS (2026-07-08): ARCS 1-5 ALL COMPLETE — program goal MET.** The Dolphin-chassis path is a
> distributable product with a verified PRIME INVARIANT. Read `2026-07-08_arc5_product-hardening.md`.
> **ARC 6 is a reserved buffer (OPTIONAL, unspent = win)** — only run it if the user asks for
> edge-hardening (FPSCR NI modes, XER carry edges, dcbz_l, feature_flags) or a future perf lever
> (inline flat-RAM memory fast path ~6%; emitter block-linking for the ~3.8× dispatch-model gap).
> There is no forced next arc; if starting arc 6, the contract below still applies to that ONE arc.

> **SESSION CONTRACT: ONE WHOLE ARC. Complete it, pass its gate, wind down once, stop.**

## The contract (binding)
You are (probably) a fresh agent. This session's job is the single next arc of Program 65, executed
COMPLETELY:
- **DO NOT DIVIDE THE ARC.** No sub-scoping, no deferring gate items, no declaring done early. The
  arc's GATE (authoritative text in `00_PROGRAM.md`) is the ONLY exit.
- **Context running low is NOT a reason to stop.** The harness summarizes and you continue. Only a
  hard blocker (needs user input, missing artifact, broken toolchain) justifies stopping pre-gate;
  then wind down naming the blocker + exact next command; the next session RESUMES THE SAME ARC.
- **One arc only.** After wind-down, stop — even with budget left.
- **Instruction override for this program:** CLAUDE.md's repo map says DolRecomp is read-only —
  that line is SUPERSEDED here. DolRecomp, GXRuntime, StrikersRecomp, dolphin-chassis are all
  ours to edit; prefer fix_generated.py over emitter changes only when genuinely cheaper. Pristine
  `dolphin/` and the other reference repos stay read-only.

## Get oriented (do these reads NOW, not "on demand" — arc 2 lost hours to facts one read away)
1. `CLAUDE.md` (repo root) — two-knowledge-systems rule + operating principles.
2. `00_PROGRAM.md` (this dir) — mission, PRIME INVARIANT, D1-D6, verified seams, arc ladder+gates.
3. `goals.json` → G016 `evidence` tail = which arc this session executes; then the newest dated
   handoff in this dir = previous session's narrative, surprises, exact next step.
4. **READ FULLY: `KNOWLEDGE/dolphin-chassis.md`** — build chain, run/observe recipes, the D3
   time-warp mechanism, verify-on-entry SMC guard, FPS-overlay semantics, chassis-specific Dolphin
   facts. **Also `KNOWLEDGE/tools.md` §Dolphin** (sendkey/hotkeys/savestates/logging). These are
   NOT optional context; every line was paid for.

## The failure modes that burned arc-2 budget — check yourself against these
1. **Stale binary.** Six artifacts across four repos. After ANY source edit, run the whole build
   block below (seconds when clean). If runtime behavior contradicts code you JUST wrote, verify
   binary freshness BEFORE forming any hypothesis — arc 2's "interpreter dominance mystery" was an
   un-rebuilt chassis binary.
2. **macOS has NO `timeout`/`gtimeout`** — and in a pipeline it fake-succeeds (exit 0 from the
   downstream command) so your smoke test silently never ran. Launch long runs with
   `cmd > /tmp/x.log 2>&1 & echo $! > /tmp/pid` and `kill $(cat /tmp/pid)`; bound guest work with
   flags (`--max-blocks`). **Absolute paths in every command** — cwd drifts between Bash calls
   (arc 2: a relative `rm -rf` cleaned the wrong dir and left 7 GB of PNGs; a relative launch
   path failed silently).
3. **Wrong observation ladder.** In order:
   a. **The user.** They are at the machine, watching, and have asked to be asked. Say exactly
      what to observe and what each possible reading would mean. Fastest tool in the box.
   b. **FPS overlay**: add `-C Graphics.Settings.ShowFPS=True -C Graphics.Settings.ShowVPS=True
      -C Graphics.Settings.ShowSpeed=True`, then ONE screenshot:
      `caffeinate -u -t 3; screencapture -x -l$(.tools/dolphin/bin/winlist | grep nogui | grep -o 'wid=[0-9]*' | cut -d= -f2) /tmp/s.png`.
      Semantics: Speed = emulated-cycle rate vs wall; VPS = VI rate; FPS = frames the GAME
      presents. `Speed 100 + VPS 60 + FPS low` ⇒ guest-time mischarge (game lag-skips
      internally), NOT GPU slowness.
   c. **Attract demo**: idle at the Strikers title ~2-3 min → a REAL match plays itself. Zero
      input needed for in-match observation.
   d. **Savestates**: sendkey 46=save, 45=load (Hotkeys.ini M/N/P) — save once inside a match,
      re-enter instantly forever after.
   e. **Framedumps LAST** (GB/min disk trap; delete `Dump/Frames` after every run).
4. **Hypothesis essays instead of one probe.** The CLAUDE.md circuit breaker is real: each new
   observation buys ONE hypothesis paragraph ending in the cheapest killing experiment. The two
   probes that solved arc 2: (i) one instrumented stderr line printing the disputed state
   (chunk idx/state/msr per fallback sample); (ii) **the CPUCore=1 control** — same binary, same
   game, `-C Dolphin.Core.CPUCore=1`, no module — separates "my core" from "environment" for ANY
   anomaly in one run.
5. **Contradicting the user's observation.** If the user reports something your metric denies,
   BOTH are usually true and the model connecting them is wrong (arc 2: TB-rate said 100% while
   the user saw 8 FPS — the D3 overcharge warped guest time). Decode their reading; don't argue.
6. **Parallel-session interference (new, arc 3).** Another session may edit product files and
   the shared task list mid-arc. Trust ONLY git + the working tree: `git diff --stat` every
   repo at session start and after any interruption; a task marked completed with no
   corresponding diff/commit is a LIE — reset it. If `generated/chunks` count ≠ 163, a foreign
   regen (e.g. 512-inst chunking) contaminated it: `rm chunks/*.c`, clean regen, RE-RUN cmake
   configure (the chunk list is a configure-time GLOB), then rebuild the module.
7. **Relative paths bit AGAIN, twice in one arc** (dead nogui launch: `env: dolphin-chassis/...
   No such file`; a ledger line written into `StrikersRecomp/runtimeharness/` — cwd had drifted
   to StrikersRecomp). Failure mode #2 is not advisory: EVERY path in EVERY Bash call absolute,
   including log/ledger appends and `rm`.
8. **Per-dispatch cost budget (arc 3's perf incident).** The burst loop runs ~80M
   iterations/sec. ANY call added per-dispatch is a regression multiplier: a single
   `GetFakeTimeBase()` there measured ~34% of the whole CPU thread and read as "downcount made
   the game 4x slower". Before blaming a mechanism, `sample <pid> 5 -f out.txt` and read the
   CPU-GPU thread tree; the verbose `charged=`/`bursts=` counters give speed, avg block size,
   and burst length without a screenshot (charged/Δwall ÷ 486M = speed).

9. **The user hand-drives the game; there is NO attract-demo to wait for (arc 5).** A
   `pc=0x80259294` (SelectThread idle spin) in the periodic counter is the idle THREAD between
   frames, NOT "stuck at a menu" — the game can be in an active user-driven match while that line
   prints. When you need an in-match scene (perf/render), ASK the user to drive to a named scene
   (heavy goal-cam vs kickoff) per failure-mode #3a — fastest instrument, they're watching. Do NOT
   wait/poll for an attract demo that isn't coming (cost budget in arc 5). Module-less
   (interpreter-only) render is CORRECT but SLOW to appear — `native=0`+no-exc+climbing `fallback`
   is the invariant proof; be patient for the visual, don't misread slowness as a hang.

## Build the world (idempotent; run after any edit)
```sh
cd /Users/aharonahdoot/Projects/GCDecomp
/opt/homebrew/opt/cmake/bin/cmake --build DolRecomp/build -j8
# ONLY after DolRecomp emitter/decoder changes — regen lands NESTED, move it up:
DolRecomp/build/dolrecomp -j8 --gamecube StrikersRecomp/generated/main.dol StrikersRecomp/generated/
cp StrikersRecomp/generated/generated/generated.h StrikersRecomp/generated/generated/generated.c \
   StrikersRecomp/generated/generated/generated_smc.txt StrikersRecomp/generated/
cp StrikersRecomp/generated/generated/chunks/*.c StrikersRecomp/generated/chunks/
rm -rf StrikersRecomp/generated/generated
python3 StrikersRecomp/tools/fix_generated.py
# Always:
cmake --build GXRuntime/build-headless -j8 && ctest --test-dir GXRuntime/build-headless | tail -2
ninja -C StrikersRecomp/build-chassis-module
ninja -C dolphin-chassis/build dolphin-emu-nogui
/opt/homebrew/opt/cmake/bin/ctest --test-dir DolRecomp/build --output-on-failure | tail -3  # expect 10/10
```
Run (module): `env STATICRECOMP_MODULE=/Users/aharonahdoot/Projects/GCDecomp/StrikersRecomp/build-chassis-module/gG4QE01_recomp.dylib STATICRECOMP_VERBOSE=1 /Users/aharonahdoot/Projects/GCDecomp/dolphin-chassis/build/Binaries/dolphin-emu-nogui -e "/Users/aharonahdoot/Projects/GCDecomp/Super Mario Strikers (USA).iso" -u /Users/aharonahdoot/Projects/GCDecomp/.tools/dolphin/user -v Metal -C Dolphin.Core.CPUCore=6`
Invariant (Melee, no module, default core): same binary, Melee ISO, drop the env var and `-C ...CPUCore`.

## Known-good arc-2 baselines (regressions are YOUR bug)
In-match native dispatches **99.88%**; `smc_failed=1` exactly (the VMBASE chunk
[0x8025D6C0,0x802616C0) — genuine SMC, EXPECTED); in-game Speed 100% / Max ~113% / ~60 FPS;
heavy cutscenes ~80% speed (the arc-3 target); THP movie renders; ball/goalie/camera correct;
Melee no-module stock-identical; DolRecomp ctest 10/10; oracle 239 cases 0 unexpected;
dedicated 26/26; GXRuntime 16/16; standalone Strikers boots post-regen.

## Arc-5 fast start (if G016 says ARC 5 — verify against 00_PROGRAM.md gate text)
- **ARC 4 IS COMPLETE (2026-07-08).** Read `2026-07-08_arc4_gate-passed.md` (gate evidence) +
  KNOWLEDGE/dolphin-chassis.md §"Arc-4 GATE PASSED". Lockstep = 0 divergences; module-less invariant
  re-proven (Melee + oracle.dol); fps/coverage documented (chassis 99.9% native, Speed 97%/heavy vs
  stock JIT 100%/Max 379%). Chassis work sits on branch `arc4-lockstep-zero` (decide merge at arc-5 start).
- **The arc-5 headline lever is the ~3.8× JIT headroom gap** = per-dispatch burst-loop overhead +
  D3 downcount batching. `sample <pid> 5 -f out.txt` FIRST (arc-3 recipe): the loop body was ≈17% of
  the CPU thread (flush+conditions+counters), chassis_dispatch subtree ≈47% = real guest work. Target:
  sorted-table binary search / direct-index dispatch (retires G011's linear `dolrecomp_call` scan in
  chassis context), interpreter-block memo for hot fallback ranges IF measured hot. Guard every emitter/
  dispatch change with the lockstep harness (`STATICRECOMP_LOCKSTEP=1`, must stay 0 divergences) + the
  oracle (239/0) + re-run fix_generated.py; version core/cpu.h ABI on any CPUState change.
- Other arc-5 gate items (00_PROGRAM.md): game-ID module packaging (one command), config toggle,
  fork upstream-sync policy, KNOWLEDGE writeup. Gate: distributable chassis + Strikers module;
  invariant statement verified. Arc-6 = reserved buffer.
- **Observation recipes are proven** (KNOWLEDGE §Arc-4 GATE PASSED §Observation): window title = core+ISO;
  CPUCore 1=JITARM64 / 6=StaticRecomp; run ONE nogui window at a time; backgrounded sleep+screencapture;
  `charged/wall÷486e6` == overlay Speed%.

## Arc-4 fast start (COMPLETE 2026-07-08 — history; gate passed, see `2026-07-08_arc4_gate-passed.md`)
- **CURRENT STATE (2026-07-07 session 2): read `2026-07-07_arc4_fcmp-fix_boot-clean.md`
  FIRST — it supersedes the lockstep-harness handoff.** Boot window is CLEAN (0
  architectural divergences). The recomp is proven correct. RESUME at the boot→match
  harness-fidelity class: (1) align the lockstep write-compare scope (native records
  gather+MMIO+**locked-cache** external writes; the interpreter sink captures only
  gather+MMIO) to clear the 13 `mmio#:N=X,I=0` GX/THP/LC reports; (2) fix loop-internal-
  end_pc alignment for the 1 strcmp off-by-one (both strings proved byte-identical — NOT a
  bug). Then boot→match to 0, then 4.7 (Melee+oracle.dol module-less) + 4.8 (fps vs
  CPUCore=1). fcmp FPCC-accumulation quirk is FIXED (both cpu.c mirrors); the 3 CTRLFLOW
  "branch splits" were the mid-accounting-block-entry undercharge (harness now grace-
  handles them; the emitter fix is an arc-5 perf item). Diagnostic:
  `STATICRECOMP_LOCKSTEP_TRACE=0x<pc>` dumps the shadow per-instruction — build/use it
  FIRST for any divergence.
- **4.3/4.4 ALREADY LANDED (2026-07-07): the FP unit is Dolphin-exact.** DolRecomp's whole FP/
  paired-single/load-store unit is now shared cpu.c helpers mirroring Dolphin bit-for-bit; the
  emitter emits calls. Oracle green (239/0 unexpected). Do NOT re-derive FP semantics — read
  `KNOWLEDGE/recomp-codegen.md` §"Whole-FP-unit rewrite" (Fill both lanes, fmr PS0-only,
  round-once FMA, ConvertToDouble/Single, host-FP-mode arm, psq LSQE-only/no-align/GQR->0/NaN->0).
- **4.5 (lockstep harness) + most of 4.6 (burn-down) LANDED 2026-07-07 — RESUME at the 8
  residuals.** Read `2026-07-07_arc4_lockstep-harness.md` (CURRENT state; supersedes the
  fp-unit handoff) + `KNOWLEDGE/dolphin-chassis.md §Arc-4 lockstep`. `STATICRECOMP_LOCKSTEP=1`
  works and is invariant-safe when off (verified). Grounding done: boot→attract-match 99.92%
  native, correct render, ~59fps (screenshot). Burn-down 100→**8**; **ZERO fpr/ps1 divergences**
  over 4120 boot blocks (FP unit value-bit-exact in-game). Do NOT rebuild the harness — extend it.
  The 8 residuals: 5 FPSCR-FPCC status (CR+fpr match; fcmp-heavy call blocks), 2 branch splits,
  1 mid-block-entry downcount undercharge (arc-3). NEXT: a temp per-instruction FPSCR/CR trace
  gated on one entry PC (e.g. 0x80251798) to pin the FP op; then widen window, drive a match,
  burn to 0; then 4.7/4.8. Run recipe in the handoff. Two self-inflicted crashes already fixed
  (PowerPCState memcpy invalid-free — it owns non-trivial iCache/dCache, save only
  msr/downcount/Exceptions; and end-detection overshoot — the cycle bound caps it).
- **Lesson bank (arc 4):** (i) feed BOTH engines identical hardware inputs (TB pin + MMIO
  read-replay + write-suppress) or a live-hardware differential is pure noise — that's the whole
  100→8. (ii) Cycle-bounding the shadow CONFLATES downcount bugs with divergences: in a CTRLFLOW
  report `N_cyc==I_cyc` ⇒ real branch split, `N_cyc!=I_cyc` ⇒ suspect the charge. (iii) When a
  long `sleep` poll gets interrupted with a `.ips` crash report, read the FULL report from
  `~/Library/Logs/DiagnosticReports/` (the pasted one truncates the CPU-GPU-thread frame).
- **Oracle after any emitter/cpu.c edit** (macOS make trap): `tests/oracle/Makefile` does NOT
  depend on emitter.c/cpu.c. `rm host_diff_gen host_generated.c host_diff dedicated_gen
  dedicated_generated.c dedicated_diff` then `make diff dedicated`, else you diff STALE emitted
  code. The 6 stswi/stswx XFAIL are a `normalize_pointer` harness artifact (USER-flagged),
  documented in `host_diff_run.c` — NOT a bug; don't chase them.
- 4.7 needs the oracle producer DOL as a 2nd module-less ISO: `DolRecomp/tests/oracle/oracle.dol`
  (a devkitPPC GC executable) plus the Melee ISO at repo root.

## Arc-3 fast start (if G016 says ARC 3 — verify against 00_PROGRAM.md gate text)
- **First 15 minutes, before real work:** add a build-stamp (`__DATE__ " " __TIME__`) to the
  chassis init + module-load stderr lines so every log self-identifies its binary — permanently
  kills failure mode #1. Then confirm baselines with one run.
- **Per-block downcount emission** (the core item): DolRecomp's `emit_function` already labels
  every instruction; emit cumulative cycle charges per block instead of the chassis's flat
  `CYCLES_PER_DISPATCH=6` (StaticRecompCore.cpp). For per-opcode costs, mirror what Dolphin's
  CachedInterpreter/PPCAnalyst charge (check `PPCTables`/`PPCAnalyst` — verify, don't trust this
  pointer). Gate says "full match at correct speed, VI-paced" — the ~80% cutscene residual is the
  measurable target; the overlay + the user are the instruments.
- **Savestate round-trip mid-match** is IN the gate: state lives in PowerPCState after SyncOut
  (bursts always SyncOut before returning to CoreTiming), but verify save/load actually lands
  outside a burst and that the module's on_state_loaded re-arms FP rounding. Use sendkey 46/45.
- **G008 differential suite**: find its definition/invocation via goals.json G008 before
  planning around it.
- Exceptions/interrupts: EXT_INT delivery is slice-granular by design (KNOWLEDGE); the arc adds
  a dispatcher back-edge check so long native stretches don't add latency — measure latency
  first, don't assume it's a problem.

## Wind-down ceremony (ONCE, at gate-pass — or at a hard blocker)
1. `goals.json` G016 `evidence`: arc result + gate EVIDENCE (numbers, log lines, screenshot
   facts), ending with **"NEXT: ARC N+1 <objective>"** (or "RESUME ARC N at <exact step>").
2. Dated handoff here: `2026-MM-DD_arcN_<slug>.md` — landed, gate evidence, surprises, next step.
3. Durable learnings → KNOWLEDGE (`dolphin-chassis.md`, `tools.md`, `recomp-codegen.md`).
   Differentiate, don't overwrite.
4. Ledger `milestone` line (goal G016).
5. Commit every touched repo (Co-Authored-By trailer; check `git status` for submodule-pointer
   noise in dolphin-chassis — exclude Externals/ drift). Do NOT touch `~/.claude`.
6. Update THIS FILE's failure-mode / fast-start sections with what the arc taught — the retro is
   part of the ceremony, not an extra.

## Budget
Median arc ≈ 405k output tokens. Key assets: ISOs at repo root (Strikers + Melee); reference
Dolphin `/Applications/Dolphin.app`; demand certificate `../64_full-compat-program/
CERTIFICATE_G4QE01.md` (specialization input later — NOT needed arcs 3-4).
