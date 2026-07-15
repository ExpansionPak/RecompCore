# Methodology — how to debug this recomp fast

Hard-won from real sessions. The recompiler + Aurora + GC SDK stack is unfamiliar terrain; these habits save hours.

## 0. Attribute the layer before touching the layer
A renderer ticket requires a discriminator artifact FIRST; "the scene looks wrong" is not a renderer ticket. Precedents: the kickoff physics collapse was lazy-FPU restore (CPU, s57); missing field/players/ball was the obsolete CP-array cap (bridge, s52); the goalie "divergence" was harness HID2 state (s53) — all looked like renderer bugs. Discriminators, cheapest first:
1. **Guest-state**: Dolphin MRAM dump vs `--dump-mem` via `tools/state_diff.py`, or `--call` differential with ambient-SPR rules (tools.md s52 recipes). Divergent guest state ⇒ CPU/codegen/HLE lane — the renderer never sees the ticket.
2. **Transform**: `DOL_AURORA_RECOMP_DRAW_TRANSFORM_LOG=1` PN/projection/viewport vs Dolphin XF memory for the same draw.
3. **Trace** (program 63 harness): digest drift vs `PRESENT_STATS` ⇒ frontend decode; digest-clean but pixels wrong vs Dolphin golden ⇒ semantics module (route per 5e).

## 1. Force-override and LOOK before hand-deriving
To test whether a suspect matrix/flag/state value is the culprit, **override it to a known-good value and observe the real renderer** — don't compute its effect on paper. Two screenshots (force identity / force scale) settled a graphics bug that ~8 hand-derivations of clip-space couldn't. An observation in the real renderer collapses a whole tree of competing hypotheses at once. Env-gated overrides (e.g. writing the guest matrix at a `GXLoadPosMtxImm` notify) are cheap and reversible (the game recomputes next frame).
- Corollary: a **symmetric** test value (identity matrix, pure scale, uniform color) can "work" even under a transpose/ordering bug. If symmetric works but the real asymmetric value doesn't, also test the asymmetric transpose to rule that in/out.

## 2. Ground every claim in the real artifact
Run it; read the actual bytes/pixels/stack. A static read of source is not observation. When two diagnostics disagree, suspect the newer/your own one first — e.g. a fresh float dump that ignored big-endian "found" garbage positions that the existing (byte-swapping) log showed were a clean quad. **Match the byte-order and conventions of the existing working logs.**

## 3. Use the decomp + lldb early, not late
`smstrikers-decomp/config/G4QE01/symbols.txt` maps every guest addr→name; `src/` has the real `.cpp`. Combined with an `lldb` write-watchpoint on a wrong value's host address, you go from "this value is wrong" to "this exact function wrote it, and here's the intended math" in a few calls. Reach for this before long black-box tracing.

## 3b. Reach for Dolphin — it answers two questions nothing else does (now INSTALLED)
Don't under-use Dolphin (`dolphin/` = source; `/Applications/Dolphin.app` = a runnable build, see `tools.md`). It's the authority on the two questions you'll keep hitting:
- **"What does GX hardware actually DO with this state?"** → read the source. The GX→screen transform/projection/viewport math (easy to hand-derive wrong) is canonical in `VideoCommon/VertexShaderManager.cpp` (`SetProjectionMatrix` ~L122, viewport + `pixel_center_correction`), `XFStructs.{cpp,h}`, `XFMemory.{cpp,h}`; the GP-FIFO opcode format in `OpcodeDecoding.cpp` + `VertexLoader*`. Check your transform/convention against this instead of re-deriving clip-space.
- **"Is this wrong value a recomp bug or the genuine HW value, and what SHOULD the screen look like?"** → *run the real ISO in Dolphin*. A screenshot (`.tools/dolphin/dolphin_run.sh`) is instant visual ground truth; launching with `-d` and watching the suspect guest address in the debugger's MemoryWidget/WatchWidget (or a MemCheck for "who writes it") tells you in minutes whether our recomp diverges from real HW — the exact question that otherwise traps you in circular reasoning. **Historical correction:** the FE view-matrix question was resolved as a recomp bug: scalar `lfs` failed to mirror the paired-single `ps1` lane, so later `psq_st` wrote stale matrix values. Do not reopen the old modelview theory. The wrong instinct (which cost a session) was treating Dolphin as useless because no binary was pre-installed — its source alone settles the HW-math questions.

## 3c. Generated code is an early branch, not a last resort
Do not assume the recompiled C faithfully implements the DOL merely because it compiles and boots. We have already found operand-rounding, split-FPR coherency, and linear-dispatch defects in DolRecomp output.

It is **not the first thing to assume**: first establish the expected game value/sequence in Dolphin or the decomp and the expected hardware behavior in Dolphin source. But it is also **never the last thing to inspect**. As soon as those oracles disagree with the recomp, or `sample` points into generated helpers/dispatch, compare the emitted C and runtime state with the original PPC instruction stream. Treat these as peer hypotheses from the start:
- game/HLE policy is wrong;
- GXRuntime hardware/PPC semantics are wrong;
- Aurora adaptation is wrong;
- generated/recompiled code is wrong.

The order is evidence-first, not layer-first: establish truth, localize the divergence, then inspect the responsible layer. Log every confirmed DolRecomp defect and upstream remedy in `recomp-codegen.md` when adding a `fix_generated.py` rule.

## 4. Stop re-deriving; run the discriminating experiment
When you notice you're computing the same thing a 3rd time, or flip-flopping between hypotheses ("is the matrix wrong or the vertices wrong or my convention wrong"), STOP and design the single observation that distinguishes them. The user will (rightly) call out rabbit-holing — treat that as an immediate signal to switch from derivation to experiment.

## 4a. Hypothesis loop — ONE analysis pass per observation (HARD RULE, the §4 circuit breaker made mechanical)
§4's "notice you're re-deriving" failed in practice: a spiraling agent never counts its own spirals
(64/Task A, 2026-07-05: three analysis passes over ONE frame-20 screenshot re-deriving decal blend
algebra — bg×(1-a) vs a×0.706 vs alpha-test-ref arithmetic — while the discriminating test was one
rebuild away; the task-list nag fired mid-spiral and was ignored, so ambient warnings don't work
either). The trigger must be observable in the TOOL STREAM, not in your self-awareness. The loop:

1. **OBSERVE** — one tool result (screenshot, counter dump, trace filter, decomp read).
2. **HYPOTHESES as table rows, not prose** — every row MUST complete this format or it's illegal:
   `H1: <claim> | KILLS IT: <experiment> | COST: <one rebuild+replay / one grep / new tooling>`
   Prose lets you theorize indefinitely; a mandatory KILLS-IT column makes an untestable
   hypothesis visibly malformed and makes "run the test" the shortest completion of the row.
3. **RUN the cheapest killer.** If any row's cost is ≤ one rebuild+replay (~2 min), writing more
   analysis is FORBIDDEN. Force-overrides in shader source (e.g. hard-code a conv frag to
   `vec4f(1.0)`, force fragment alpha to 1.0) are the canonical cheap killers on the render path —
   one such run kills whole theory families.
4. **GOTO 1** with the new result. The mechanical spiral check, applied before writing ANY
   analysis: *is there a new tool result since my last hypothesis paragraph?* No → you are
   spiraling by definition; run the experiment your last paragraph already named.

Worked example (same incident): FragA8:=const 1.0 → decals render as full rectangles → copy→
convert→bind→decal path proven correct in ONE run; forced fragment alpha:=1.0 → black rectangles →
draw-alpha-reaches-EFB proven in ONE more. Two runs replaced ~30% of a session's tokens worth of
blend algebra, and located the real cause (projected-texgen lightramp sample) in the process.

## 4b. Compress the fault tree before burning runs
When a bug could be texture binding, display-list playback, array spans, matrix state, or Aurora impedance, do not test those as a serial checklist by default. First write a compact fault tree and ask: **what one override/log/capture would separate the most branches?**

Actionable pattern:
1. Name the competing branches and the distinct visual/log signature each predicts.
2. Pick a coarse, reversible knob that stress-tests the shared boundary (example: raise `STRIKERS_MAX_ARRAY_VERTS` after span-aware uploads, or emit all resource metadata before raw display-list playback).
3. Define the success/failure interpretation before launching the run.
4. Capture one screen/log at the discriminating scene, then update the tree: solved class vs newly exposed class.

Do this especially after two small experiments fail or when the user says the debugging feels like too many experiments. The Strikers gameplay breakthrough came from treating the font-atlas explosion, display-list metadata, and indexed-array span as one boundary problem, then using a single high-span run to prove the old 2048 cap was scene-limited.

## 5. Form competing hypotheses, then kill them
Per the fablize investigation pack: reproduce first, list 3+ competing causes, gather evidence per cause, trace the full causal chain, verify before/after, and record which hypotheses you *rejected* (so the next agent doesn't re-chase them).

## 5b. Run two loops: conformance and client acceptance
The harness is not a queue of Strikers screenshots, and it is not a games-free emulator project.
- **Conformance loop:** executable PPC differentials, core/device tests, synthetic DOLs, fake-backend transcripts, and GX trace replay.
- **Acceptance loop:** deterministic Strikers scenes, then a second title.

Classify a divergence first. If it exposes a cross-cutting primitive, stop the scene chase for a bounded runtime task, prove the primitive independently, then return to the same scene. If it is genuinely title policy/middleware, keep it in Strikers and do not manufacture a generic abstraction.

Closure rule: a reusable fix needs (1) before/after client evidence and (2) the smallest practical non-game regression. If isolation is not yet possible, record that test debt explicitly; "the current scene looks right" is not a reusable-runtime proof.

Conformance is a method, not an excuse to fossilize a provisional boundary. Before creating a broad recorder or public API around an adapter, identify who should own the semantics in the collaboration end-state. Example: Aurora already parses CP/BP/XF/draw FIFO state, so a GXRuntime recorder around `set_array/load_texture/load_tlut` would preserve today's native-pointer workaround, not prove a neutral runtime contract. Settle the guest-address resolver boundary first, then test it.

## 5c. Separate workaround location from semantic ownership
Read-only references force some fixes into the nearest writable adapter. Record both facts:
- where the workaround must live **today**;
- where the behavior should live if the projects collaborate.

`GX_CULL_ALL` is the canonical example. GXRuntime currently rewrites FIFO state because Aurora is read-only and fatals on cull mode 3. Dolphin shows that cull-all is a draw/vertex-processing semantic, and Aurora owns that command processor, so the collaboration remedy belongs in Aurora. Do not infer permanent layer ownership from the writable repository.

## 5d. Use Dusklight as the Aurora-native driving oracle
When the suspect seam is "how should a game drive Aurora?" inspect `dusklight/` before blaming Aurora or designing a new runtime API. Dusklight is not hardware truth; Dolphin is. Dusklight is source-port truth for Aurora ownership and call paths.

Actionable checklist:
- Find the Dusklight call path for the same concept (`GXCallDisplayList`, `GXSetArray`, `GXLoadTexObj`, FIFO writes, matrix loads, draw submission).
- Identify whether Dusklight emits retail FIFO bytes, Aurora extension commands, or source-level Dolphin GX wrappers.
- Compare that path to Strikers' recomp path and record the mismatch as one of: missing metadata, guest-address/native-pointer impedance, unsupported retail FIFO opcode, or our HLE policy.
- If Dusklight and Strikers both converge in Aurora's command processor, stop rewriting Strikers toward source-level GX; fix the bridge/seam instead.
- If the local fix belongs in a writable shim only because `aurora/` is read-only, log the upstream Aurora remedy and the local workaround together.

## 5e. Renderer work is module-driven, not scene-driven
Strikers may reveal renderer gaps, but Aurora recomp mode should advance like GXRuntime device work: one module, one focused fixture/replay, one acceptance check. Do not wait for gameplay traversal to discover every missing renderer contract.

Actionable rule: before another Strikers graphics experiment, map the symptom to one Aurora recomp-mode module: guest resolver, FIFO ingress, display-list executor, CP array bases, indexed range analyzer, texture/TLUT resolver, XF/BP/TEV/cull conformance, or trace replay. If the proposed fix is "emit more native metadata from Strikers HLE," write the fixture/API for the Aurora module first unless the change is strictly a temporary bootstrap.

Use the N64ModernRuntime comparison correctly. Its scalable lesson is a stable
runtime/recomp bridge plus a renderer callback seam, not a promise that a
general runtime magically removes renderer work. For GameCube, the renderer
seam must be retail GX FIFO/resources/PE/VI. When an agent is tempted to chase a
Strikers scene, force the work back into this shape: module -> oracle fixture ->
normalized transcript -> Aurora sink packet/image -> Strikers acceptance.

## Known time-wasters (don't repeat)
- **macOS black screenshots** → display sleep; `caffeinate -u` first (see `tools.md`). Cost a prior session ~25 tool calls chasing Spaces/Metal/ScreenCaptureKit. This is the canonical "should've checked the environment first" lesson.
- **Reading big-endian guest floats as little-endian** → phantom "garbage" bugs.
- **Re-deriving clip-space transforms by hand** instead of force-overriding.
- **Suspecting Aurora's "display copy is not implemented" stub** — proven harmless across 3 sessions.
- **Reading a `--call` single-function `PROGRAM` fault at `blocks=0` as a codegen bug** (cost session 52 a
  whole "goalie diverges" lead). A bare single-function harness skips boot, so `HID2[PSE|LSQE]` is clear and
  the first paired-single op faults via `psq_check_enabled` — a harness-fidelity artifact, not a recomp
  divergence. Establish ambient SPR state (HID2 default is now built into `--call`; `--restore <savestate>`
  for real GQRs) BEFORE concluding anything. Corollary: when an isolated-call MEM1 diff shows a region the
  function "should" have written but didn't, do the 3-way `in`/`recomp_out`/`dolphin_out` byte check — the
  Dolphin oracle's Step-Out can fold in interrupt/other-task writes your isolated call correctly omits, so
  `in==recomp_out` there means *your* run is right, not wrong.

## 6. Treat timing as one clock-domain change
When a block-count cadence changes, grep every peripheral scheduler before declaring pacing fixed. Historical example: VI at 350k blocks/frame made the FE smooth, while AI initially still used a 600k-block interval derived from the former 2M VI cadence. That mismatch is invisible in static menus and becomes obvious in THP, where audio-ring fullness can gate video decode. Re-test timing-sensitive scenes, not only the scene used to tune the clock.

When `sample` attributes a large share directly to the host `main` loop, resolve offsets before optimizing the current guest function. In the THP case, 2,448/4,045 main-thread samples were the inlined 163-range `dolrecomp_call` chain, not THP itself. Replacing it with indexed chunk dispatch dropped `main` to 256 samples and restored sustained 60/30 movie cadence. This is the canonical example of a codegen dispatch tax masquerading as a game decoder/audio bug.

For input-sensitive reference paths, do not automate away the observation. The scripted Dolphin run missed/skipped the movie; the useful experiment is a normal boot with the user driving while the debugger remains available.

Current audio caution: the older "160 samples every 5 ms = 105k dispatches" sketch was superseded by chunk-level `DolAudioDma` (32-byte AID chunks, 8 stereo frames). Do not assume a rate switch because movie audio sounds wrong. Strikers observed AI control values `0x42`, `0x62`, and `0x46`; all keep AID at 32 kHz. If audio disappears, first check for SDL/CoreAudio open/start errors and AID chunk peaks before changing the emulated rate.

## GP FIFO Stream Alignment Debugging Traps
- **Do not intercept raw size-4 writes without verifying the opcode**: All writes to WGPIPE (`0xCC008000`) pass through the same channel. This includes BP register writes (command `0x61`) as well as raw vertex coords and color/tex attribute floats during drawing. Intercepting or modifying size-4 data based on bit values without validating that the preceding command was indeed a register write (e.g., `s_last_opcode == 0x61`) will corrupt the vertex stream, resulting in memory overruns/EXC_BAD_ACCESS in `copy_xf_data` or shader compilation errors (e.g., `unmapped vtx attr 9`).
- **Maintain sequence order when inserting writes**: The command processor expects parameter writes to complete their active opcode command. Before injecting any additional custom commands (such as a separate `0x61` write), the pending guest parameter must be flushed to the FIFO first. Injected writes must also explicitly write the command opcode byte (like `0x61`) before the parameter.

## GX accuracy gates = gxpo (Program 68), not eye / host SSIM / F1–F3
- **G-GX accuracy language (66 §12):** "gxpo G0–G5 green on the declared corpus".
- Exact keys or refuse; same plane or refuse; planted defects gate tiers; INVALID on missing
  preconditions (never silent skip-pass). Recipes: `KNOWLEDGE/tools.md` §gxpo + `GXRuntime/docs/gxpo.md`.
- Host screencapture / banded SSIM / eye are **triage only** — never close accuracy units.
- Quarantined F-tools (`gp_state_diff.py`, `pixel_diff.py`, `graphics_lockstep.sh`,
  `dolphin_efb_golden.sh`) are frozen failure-mode evidence (DR-O10) — do not extend.
- First real retail red → fix unit on Program 66's queue with the red `certificate.json` (DR-O11).

## Multi-agent windows (Program 66 orchestration)
Full playbook: `handoffs/66_certificate-runtime/02_ORCHESTRATION.md`. Durable rules:

- **Default 2 product lanes**, not 3. Spawning is expensive; pad lanes waste tokens.
- **Opencode per-lane budget ≤ ~125k** output (window ~300k). **One whole unit per lane per window**, then STOP — even if budget remains. No second unit on the same lane.
- **impl-a** = §10 spine (gxcore U-queue / U9). **impl-b** = a different §10 workstream with disjoint files (B*, C*, E*, fork binds) — real plan, not busywork.
- **impl-c only if** the unit is itself on the plan (live acceptance, Dolphin A/B for a named gate, or tools batch ≥ mid) **and** a+b already have full batches. **Never** spawn c just to hit 3 (obs briefs, redundant remeasures, gap tourism).
- Litmus: if dropping the lane would not slip §10/§12, don’t spawn it. Solo unit → do it yourself, don’t open one orchestrated lane.
- Private build dirs + class locks (`gui-live` / `dolphin-oracle`); pre-parent baseline before blaming peers.

## Pipes eat exit codes — never `cmd | tail` when the code matters (2026-07-10)

Twice in one session: (1) `gxpo_run --corpus | tail -40` hid the corpus aborting early (its exit
2 became tail's 0, and the log kept only the last 40 lines — the run-dir listing, not the log,
revealed only 4/13 fixtures ran); (2) `ninja … | tail -2` hid a COMPILE FAILURE — a stale oracle
binary then produced a phantom cross-side red that cost a full diagnose loop (fog_params G2 red
with bp_regs equal = one side running old derivation code). Rule: redirect to a file
(`cmd > log 2>&1`), check `$?`, THEN inspect the file. In scripts, `pipefail` — interactively,
no pipes on build/run commands. Corollary: after any "background build succeeded", verify the
artifact's mtime is newer than your last edit before trusting results produced with it.
