# KNOWLEDGE base — GXRuntime + Strikers development

`runtimeharness` is the markdown control plane that drives agents. It owns durable knowledge and `.fablize` state; it does not ship runtime code.

The product repositories are separate:
- `GXRuntime/`: game-agnostic static-recomp runtime, GameCube hardware model, HAL, platform backends, and the GXRuntime-owned Aurora recomp-mode adaptation path.
- `StrikersRecomp/`: generated Strikers code, G4QE01 symbols/addresses, game HLE policy, and game diagnostics.

Strikers is GXRuntime's first development client, not its identity. Strikers exposes missing hardware/renderer behavior; the resulting implementation enters GXRuntime or Aurora recomp mode through a game-independent contract. GXRuntime improvements are immediately proven against Strikers, but renderer modules are developed with focused FIFO/resource fixtures so correctness does not depend on discovering bugs scene-by-scene.

> **Dolphin is INVALUABLE ground truth. Use it early.**
>
> The real ISO in Dolphin establishes the correct screen, timing, audio, input, and guest values. Dolphin source establishes hardware behavior. The recomp is the implementation under test and cannot validate itself. Read `dolphin-ground-truth.md` before diagnosing a new subsystem.

| File | What's in it |
|---|---|
| `architecture.md` | Repository ownership model, current Strikers integration map, and how Strikers drives GXRuntime without owning it. |
| `gxruntime.md` | Game-agnostic runtime/HAL contracts and extraction rules. |
| `dolphin-ground-truth.md` | Mandatory oracle workflow for visuals, memory, timing, GX, and hardware behavior. |
| `aurora-runtime.md` | Aurora/GX engine quirks and GXRuntime-owned Aurora recomp-mode design: real-GC FIFO, guest-memory resolver, display lists, indexed spans, texture/TLUT resources, trace replay. |
| `recomp-codegen.md` | DolRecomp defect ledger, upstream remedies, `fix_generated.py`, big-endian guest memory, paired-singles, indexed dispatch. |
| `tools.md` | Exact recipes: build, run, screenshot (caffeinate), `lldb` watchpoints, `sample`, objdiff. The macOS gotchas. |
| `dolphin-chassis.md` | Program 65 owned Dolphin fork: StaticRecomp core build/run/module recipes, chassis-specific Dolphin facts (lazy TB, slice-granular EXT_INT), lldb-on-chassis traps. |
| `methodology.md` | How to debug this thing fast: observe-don't-assume, force-override-and-look, use the decomp early, known time-wasters. |
| `prompts.md` | The canonical session-START and session-END prompt templates. |

## How to contribute knowledge (keep it useful)
- **Classify ownership first.** Use this routing table:
  - Console/PPC/runtime/HAL behavior -> `gxruntime.md`.
  - Repo ownership, Strikers integration, G4QE01 addresses/scene facts, and durable Strikers status -> `architecture.md`.
  - Aurora/GX backend quirks and Aurora recomp-mode module design -> `aurora-runtime.md`.
  - DolRecomp/generated-code/PPC semantic quirks -> `recomp-codegen.md`.
  - Commands, scripts, env vars, and exact recipes -> `tools.md`.
  - Debugging habits, recurring traps, and corrected methodology -> `methodology.md`.
  - Dolphin oracle discipline -> `dolphin-ground-truth.md`, with exact commands in `tools.md`.
  - Current hypotheses, exact next step, and active bug state -> `../goals.json` plus a handoff, not KNOWLEDGE.
- **Keep the file set small.** Add a bullet/section to the routed file rather than creating a new file. If two files overlap, merge toward the more operational one. A smaller harness beats a perfectly taxonomized one that agents forget to update.
- **Terse and concrete.** Prefer the exact command/address/value over prose. Include the *why* when it's non-obvious.
- **Differentiate, don't delete — a failure in your case is a NEW DATA POINT, not a refutation.** Before overwriting/removing a recipe because it didn't work for you, decide which it is: *flat wrong* (the belief is provably false → strike/correct it) vs *it worked under conditions you don't have* (→ KEEP it, label its conditions, and ADD your case beside it). **Default to keeping.** Almost every engine/tool fact here is context-dependent — which screen, which build (GUI vs headless), macOS state (display asleep? Accessibility?), game phase (FE vs gameplay), which input layer (session vs HID). Tag each solution with *when it applies* so the next agent picks the right branch instead of re-litigating. Example: "`osascript keystroke` is invisible to Dolphin (HID polling); `sendkey` at the HID tap works" — both halves are worth keeping; deleting the first loses the *why*.
- **Only strike a line when the belief itself is false**, not merely "didn't reproduce for me." When unsure, keep both and note the divergence — we grow by accumulating differentiated cases, not by overwriting.
- **Durable only.** Current-bug state and "next step" go to `../.fablize/`, not here.


# PLEASE CONTRIBUTE. WE NEED YOUR HELP TO GROW!
