# Session prompts

Copy/paste these at session start or wind-down. These prompts are designed to control
the agent's process, not to re-state the whole knowledge base. Keep the current focus
short; the agent should get facts from `goals.json`, the newest handoff, and the routed
KNOWLEDGE file that is relevant to the next decision.

Paths assume the workspace root is `/Users/aharonahdoot/Projects/GCDecomp`.

---

## START-OF-SESSION

```
Make a fully playable version of Super Mario Strikers using Aurora: graphics, input, and audio.
MacOS is the active build/test host, but keep the runtime design OS-agnostic.

Repository ownership:
- runtimeharness/ is the markdown control plane only. It owns durable knowledge and .fablize state.
- GXRuntime/ is the reusable static-recomp runtime: PPC/core, guest memory, hardware services, HAL, platform backends, and GXRuntime-owned Aurora recomp-mode modules.
- StrikersRecomp/ is the game client: generated Strikers code, G4QE01 symbols/addresses, game HLE policy, diagnostics.
- Standalone reference repos are READ-ONLY: aurora/, dusklight/, dolphin/, smstrikers-decomp/, DolRecomp/.
- Recomp-specific Aurora changes belong under GXRuntime/third_party/aurora-recomp/ first, then a GXRuntime-owned Aurora fork/subtree/module once stable. Do not commit them to standalone aurora/.

Before doing implementation work:
1. Read ./CLAUDE.md.
2. Read runtimeharness/.fablize/KNOWLEDGE/README.md.
3. Read methodology.md closely enough to follow its control-flow.
4. Skim only the indexed knowledge needed for the current task. Always include:
   architecture.md, gxruntime.md, tools.md, and whichever of dolphin-ground-truth.md,
   aurora-runtime.md, recomp-codegen.md, or other routed files owns the suspected layer.
   Do not try to solve the task by loading every token and improvising from memory.
5. Read runtimeharness/.fablize/goals.json and the newest file in
   runtimeharness/.fablize/handoffs/ for the exact current state and next step.
   Read all of goals.json only at the START of a session/arc. When continuing
   micro-steps inside an arc you already have context for, do NOT re-read the
   whole file -- the active goal's entry plus the newest handoff/ledger line are
   enough. goals.json is large; re-reading it every micro-step is the main
   wind-up cost this cadence exists to avoid.
6. While reading, check whether any knowledge file is stale. If you find a corrected
   durable fact or tool recipe, update the right file under
   runtimeharness/.fablize/KNOWLEDGE as part of the work.

Work in ARCS, not micro-steps. An arc is a coherent batch of related changes that
meaningfully advances ONE goal/subgoal (e.g. several Aurora recomp-mode modules in
sequence, or a device plus its tests plus its Strikers consumer). Run the full
wind-down ceremony (summary, handoff, goals.json evidence, knowledge sweep, commits,
exit/start-prompt rereads) ONCE PER ARC, or when context runs low, or when you switch
to a different goal -- NOT after every change. Within an arc, checkpoint a micro-step
cheaply: a one-line append to runtimeharness/.fablize/ledger.jsonl (event:"checkpoint")
and an opportunistic KNOWLEDGE edit when you actually learn something durable. Then keep
building the next micro-step without re-reading the prompts or all of goals.json. Commit
freely at natural checkpoints (commits are cheap; it is the narrative ceremony that must
amortize). End the arc on a VERIFIED milestone, not mid-experiment.

Process discipline:
1. Restate the exact next step in one or two sentences before editing code.
2. Classify the work:
   - Runtime conformance: PPC/codegen, loader/boot, memory, device, event-clock, ABI,
     backend-neutral host service, or synthetic-DOL/probe test.
   - Aurora recomp-mode module: guest resolver, retail FIFO, display-list executor,
     CP array bases, indexed spans, texture/TLUT/image resolver, XF/BP/TEV/cull
     conformance, or FIFO/resource trace replay.
   - Client acceptance: a deterministic Strikers scene/regression proving the game moved
     closer to playable.
   - Title policy: G4QE01 addresses, SDK symbol choices, middleware quirks, diagnostics,
     or scene automation that should remain in StrikersRecomp.
   If the classification is unclear, gather the smallest observation that separates them.
3. Before committing to a path, create an option set:
   - Name at least two plausible approaches when the next step is architectural, uncertain,
     or has already consumed more than one failed attempt.
   - Include one "boring/local" option, one "layer-boundary/runtime" option, and one
     adversarial option that assumes the current theory is wrong when those categories apply.
   - For each option, write the evidence that would support it, the fastest falsifying test,
     the blast radius, and how reversible it is.
   - When symptoms could share a boundary, compress the fault tree: design one observation or
     override that separates several hypotheses at once. Do not run a long sequence of tiny
     one-variable experiments until you have first asked what single run, source comparison,
     or capture would make multiple branches lose.
   - Pick the option with the best evidence-to-risk ratio, not the one that best matches the
     current narrative. If two options are close, run the cheapest discriminating experiment
     before editing production code.
4. Establish the oracle before chasing symptoms:
   - Dolphin run/debugger for what the game does and what the screen/audio/input should be.
   - Dolphin source for hardware behavior.
   - smstrikers-decomp for title intent and symbol/address truth.
   - dusklight for how an Aurora-native game drives Aurora at the same seam; use it to compare
     call paths and ownership, not as hardware truth.
   - The recomp binary/generated C for implementation truth when it diverges.
5. Pick the narrow loop:
   - For reusable behavior, write the smallest non-game GXRuntime test/probe that can
     fail before and pass after, then prove Strikers still advances.
   - For Aurora recomp-mode work, build the focused FIFO/display-list/resource fixture
     before relying on another gameplay traversal. Strikers is the acceptance client,
     not the renderer's only discovery tool.
   - For Strikers-only policy, keep literals and diagnostics in StrikersRecomp and avoid
     manufacturing a generic abstraction.
   - For Aurora impedance mismatches, capture any bootstrap patch under
     GXRuntime/third_party/aurora-recomp/ and move toward a guest-memory resolver /
     retail-FIFO API; do not freeze provisional set_array/load_texture/load_tlut-style
     metadata as the permanent GXRuntime API.
6. Implement the minimal change, verify it in proportion to risk, and remove noisy
   diagnostics before calling it done.
7. Record the result. At each MICRO-STEP, this is cheap: append one ledger.jsonl
   line and patch a KNOWLEDGE file only if you learned something durable. The full
   record (goals.json evidence + a dated handoff narrative) is an ARC-BOUNDARY
   action -- write it once per arc, not per change:
   - Durable fact/tool/method correction -> the right KNOWLEDGE file (anytime).
   - Current state, rejected hypotheses, and exact next step -> goals.json plus a
     handoff (at arc wind-down).

Escape hatches for local minima:
- If two consecutive attempts fail, stop adding patches and widen the option set.
- If profiling, Dolphin, decomp, and generated code disagree, trust no single layer; design a
  cross-check that can make one layer lose.
- If the current plan requires preserving a large workaround, ask whether the seam is wrong.
- If a "generic runtime" task starts looking like a Dolphin rewrite, return to a client-visible
  acceptance slice and prove only the primitive currently needed.
- If a Strikers fix cannot be stated as either title policy or reusable console behavior, pause
  and reclassify before editing.

Ground claims in the real artifacts: Dolphin for game truth, Dolphin source for hardware truth,
the recomp binary for implementation truth. Use lldb, sample, caffeinate+screencapture, and the
recipes in tools.md. For the intro movie path, launch normal Dolphin with -d and let the user
drive; the FE save-state helper can bypass the movie.

Treat generated/recompiled code as an early peer hypothesis, not a last resort. First establish
expected game/hardware truth, then inspect emitted C/PPC semantics immediately when the recomp
diverges or profiles into generated dispatch/helpers. Every new fix_generated.py rule must add a
named DolRecomp defect and proper upstream remedy to recomp-codegen.md.

Do not choose either extreme:
- Do not rebuild a complete games-free runtime before running games.
- Do not patch Strikers scene-by-scene without extracting cross-cutting console behavior.
Use client-driven conformance: Strikers is the first acceptance client, while GXRuntime gains
isolated tests and game-independent contracts for reusable behavior. For renderer work, use
module-driven Aurora recomp-mode fixtures before Strikers scene smokes. Bring in a second title
early enough that no single-client interface is called stable.
Use the N64ModernRuntime analogy as a boundary lesson: stable runtime/recomp bridge plus renderer
callback seam. For GameCube, the renderer seam is retail GX FIFO/resources/PE/VI, not Aurora
source-native metadata.

Do not use external persistent notes or write under ~/.claude. This project's durable notes and
handoffs live only in runtimeharness/.fablize/KNOWLEDGE and runtimeharness/.fablize.
```

---

## CURRENT-FOCUS ADD-ON

Use one or two lines after the start prompt when a bug needs extra emphasis.

```
Current focus: resume runtime work. Session 61 completed the DolRecomp full-state compatibility
pipeline: 240 strict Interpreter64 captures, 239 emitted-C cases (99 known XFAIL, 0 unexpected,
0 XPASS), 26 dedicated control/system cases, and complete Strikers+Melee opcode inventory.
```

---

## END-OF-SESSION / WIND-DOWN

```
This is the ARC wind-down. Run it ONCE PER ARC -- at a verified milestone, when context runs
low, or when switching goals -- not after every micro-step. One handoff and one goals.json
evidence update should cover the whole arc's micro-steps (which were checkpointed in
ledger.jsonl along the way), not one handoff per change.

Capture everything for the next agent. Append where possible; correct stale
lines only when the old claim is now proven wrong. Match each file's terse tone.

First write a one-screen outcome summary:
- What changed.
- What was verified.
- What is still broken.
- The exact next command/experiment/edit.
- Which hypotheses were rejected and why.

Write into THIS repo's control plane:
- Durable learnings -> runtimeharness/.fablize/KNOWLEDGE/
  Put engine/runtime quirks, tool recipes, corrected theories, Dolphin facts, and methodology in
  the right knowledge file. Before finishing, check whether any knowledge file you read, relied on,
  or made stale needs an update; patch it now.
- Current state -> runtimeharness/.fablize/
  Update goals.json evidence/notes and add a dated narrative under
  runtimeharness/.fablize/handoffs/.

Split ownership correctly:
- GXRuntime facts belong in gxruntime.md.
- Strikers integration, addresses, scene facts, durable status, and game-policy facts belong in
  architecture.md or the handoff depending on durability.
- Aurora/GX backend quirks belong in aurora-runtime.md.
- DolRecomp/generated-code/PPC semantic quirks belong in recomp-codegen.md.
- Commands, scripts, env vars, and exact recipes belong in tools.md.
- Debugging habits and recurring traps belong in methodology.md.
- Dolphin oracle workflows belong in dolphin-ground-truth.md or tools.md.
- Current hypotheses and exact next step belong in goals.json plus the newest handoff.

Prefer one correct routed update over touching many files. If a durable fact does not fit one of
the routed files, add a small section to the closest operational file rather than creating a new
knowledge file.

Do not fossilize provisional interfaces in the handoff. If a fix lives in a writable adapter only
because a reference repo is read-only, say both:
- where the workaround lives today;
- where the semantic owner should be in a real DolRecomp/Aurora/GXRuntime collaboration.

Do not write under ~/.claude. Do not put runtime code in runtimeharness. Mention any uncommitted
code state that matters, especially dirty StrikersRecomp/ or GXRuntime/ files.
```

---

## Maintainer Notes

- Good harness prompts are procedural and selective. Large context primes terminology, but it
  does not reliably make an LLM apply a methodology. The start prompt therefore forces a
  control-flow: load current state, classify ownership, generate competing options, establish
  oracle truth, pick the smallest conformance/acceptance loop, implement, verify, and route notes.
- Procedural does not mean rigid. The option-set and escape-hatch steps are there to prevent
  local minima: when evidence is weak, attempts fail, or a seam smells wrong, the agent must widen
  the search and make ideas compete before it edits more code.
- The prompt names explicit in-repo paths because path drift caused stale `./KNOWLEDGE` and
  `./.fablize` writes after `runtimeharness/` became the control plane.
- The start prompt requires checking knowledge freshness up front; the wind-down prompt requires
  a final routed stale-knowledge sweep. This keeps durable files from lagging behind handoffs
  without asking agents to edit every file.
- Keep task-specific facts in the add-on, not in the base prompt, so this file does not become a
  second handoff.
- Keep the add-on honest. When the newest handoff changes the next step, update this file's
  current-focus example or it becomes a stale fossil that pulls future sessions backward.
- Slice scope (added after 44 sessions / 37 handoffs): the wind-up/wind-down ceremony has a
  large fixed token cost (re-reading the ~26k-token goals.json, writing a full dated handoff,
  a summary, and exit/start-prompt rereads). When each slice is one tiny module, that ceremony
  can dwarf the real work. The fix is the two-tier cadence above: an ARC bundles several related
  micro-steps and pays the ceremony once; micro-steps are checkpointed by a one-line ledger.jsonl
  entry plus opportunistic KNOWLEDGE edits. This does NOT weaken the guarantees -- every durable
  learning still lands in KNOWLEDGE before context ends, goals.json still reflects reality at each
  arc boundary, every arc still produces a handoff and committed working state. It only stops
  paying the narrative tax per change. Sessions 31-44 (one Aurora-recomp arc spread across ~14
  per-slice winddowns) are the motivating example: they should have been ~3-4 arcs.
- Recommended /goal directive phrasing that embodies wider arcs (copy when re-issuing the goal):
  "Complete the runtime library. Work in arcs: bundle several related micro-steps that advance one
  goal, checkpointing each micro-step with a one-line ledger.jsonl entry and KNOWLEDGE edits as you
  learn. Only at an arc boundary (a verified milestone, low context, or a goal switch) reread the
  exit prompt, summarize, wind down, and commit each changed repo (runtimeharness/GXRuntime/
  StrikersRecomp) in the existing 'Session N + <desc>' format; at the start of a new arc reread the
  start prompt. Log any solution that needs an upstream aurora/DolRecomp fix to KNOWLEDGE with both
  the upstream remedy and the local workaround. Never end your message; continue arc to arc."
