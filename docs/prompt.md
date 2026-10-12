  You are helping me simplify and convert a STEP CAD file to MCNP/CSG using csgify.

  Project rules:
  - Do not silently approximate geometry.
  - Prefer explicit, replayable operations.
  - Use analysis/proposal commands first.
  - Apply only high-confidence automatic changes.
  - If a decision is ambiguous, stop and ask me with the relevant solid/face IDs and viewer command.
  - Preserve volume as much as possible and report volume deltas.
  - Removal, decomposition, surface replacement, and hole filling must be logged.

  Input STEP file:
  rails.stp

  Goal:
  Produce an MCNP/CSG output file suitable for neutronics, after simplifying/removing irrelevant components, replacing supported spline/unsupported surfaces,
  decomposing non-convex solids, and checking volume consistency.

  CLI conventions (post-UX update):
  - Per-verb help is available via `csgify <verb> --help` (also `help <verb>`); each verb prints its mutation contract.
  - `--report -` writes JSON to stdout, so you can pipe into `jq`/Python instead of writing temp files.
  - The `propose <kind>` / `apply <kind>` pair is the unified front door over candidate generation/application; `<kind>` is one of: decompose | surface | removal | hole-fill. The legacy `decompose-suggest` / `remove-suggest` / `surface-suggest` / `hole-fill-suggest` verbs still work but emit a `deprecated_verb` info diagnostic.

  Workflow:

  1. Inspect the model:
     ./home/giovanni/projects/csgify inspect <STEP> --report report.initial.json
     ./home/giovanni/projects/csgify inspect-tree <STEP>

     Validate the `units` block in report.initial.json before treating volumes as mm^3.
     If `length_unit` is empty or unexpected, re-run with `--units NAME` (assert without
     rescaling) or `--normalize-units NAME` (actually rescale geometry; logged as a
     decision).

     Check `report.overlaps` for interpenetrating solid pairs. MCNP cannot handle
     overlapping cells; ANY pair (pre-existing or introduced by a later op) must be
     resolved (decomposition, removal of the stray solid, or fixing the source CAD)
     before emit. csgify now refuses to write the MCNP deck while any pair exists
     -- emit, the default convert, and preprocess --safe-all all abort with the
     `solid_overlap` (or `mutation_introduced_overlap`) error in the diagnostics.
     Stop and report the pair list to me; do not pass --allow-overlaps without my
     explicit confirmation. On very large assemblies, `--no-overlap-check` skips
     the analysis only if I have independently confirmed there are no overlaps.

     Every mutation (replace-solid, decompose, fill-hole) also runs a scoped overlap
     check on the newly-created geometry. A `mutation_introduced_overlap` error means
     the replacement bbox / cylinder / piece is now occupying space where another
     solid already lives. Back out the op (or tighten the volume guard) before
     continuing -- the next emit will refuse until the new overlap is gone too.

  2. (Optional, fast opening pass) Apply every safe op in one command:
     ./home/giovanni/projects/csgify preprocess <STEP> --safe-all --out-step simplified.step --log decisions.json --report report.safe_all.json

     `--safe-all` composes explicitly bounded auto-fit of qualifying spline faces
     and high-confidence BSP decomposition of non-convex solids. Feature removal is
     a modeling-policy decision and stays opt-in via
     `--apply-removal-candidates high|medium`; hole fills also stay opt-in. Use
     `--dry-run` first to see the plan without mutating; the closing summary lists
     what's left for me.

     If you used this step, re-run `inspect` on `simplified.step` and continue the
     loop against that file. Otherwise continue from <STEP>.

  3. Suggest removable components:
     ./home/giovanni/projects/csgify propose removal <STEP> --report removals.json

     Review candidates. Automatically remove only high-confidence small hardware/cosmetic components if they are clearly irrelevant. Do not remove anything named or
  classified as shielding, absorber, detector, source, fuel, boron, cadmium, hafnium, etc. Ask me before removing ambiguous components.

     For visual triage of the whole set at once, ask me to run:
     ./home/giovanni/projects/csgify view <STEP> --review-removals --log decisions.json --out tmp.i
     (the Qt frontend groups candidates by assembly occurrence, shows blockers and exact solid counts,
      and applies the checked set asynchronously as one certified transaction and one undo step. With
      --log the removal is replayable; with --out the deck is emitted on quit.)

     You (the agent) can also drive the SAME review headlessly, no window needed, by replaying a key
     string — useful to apply a vetted set of decisions and capture the log:
     ./home/giovanni/projects/csgify view <STEP> --review-removals --keys "A,Tab,A,Q" --log decisions.json --out tmp.i
     (the headless controller retains sequential keys: A=accept active, X=reject,
      Tab/BackTab=cycle, R:solid:N=remove a specific solid, Q=quit+commit.)

     To apply a removal:
     ./home/giovanni/projects/csgify apply removal solid:N <STEP> --out tmp.i --log decisions.json --report report.after_removal.json

  4. For each removed component that may leave a relevant hole, check hole-fill candidates:
     ./home/giovanni/projects/csgify propose hole-fill <STEP> --host solid:N --near-removed solid:M --report holes.json

     Ask me before filling holes unless the candidate is obvious, small, and high confidence.

     To apply a hole fill (target is the host solid; candidate id comes from the propose report):
     ./home/giovanni/projects/csgify apply hole-fill solid:N <STEP> --candidate hole-fill:K --out tmp.i --log decisions.json --report report.after_hole_fill.json

  5. Find unsupported/spline faces and explain blockers:
     Read report.initial.json. For every face with:
     - needs_surface_replacement = true
     - needs_spline_replacement = true
     - needs_emitter = true

     Or, to get a focused human explanation for a single target (solid:N, piece:N, or face:N.M):
     ./home/giovanni/projects/csgify explain solid:N <STEP>
     ./home/giovanni/projects/csgify explain face:N.M <STEP>

     `explain` prints the blocker reason, suggested action, and the top few candidates;
     exit code 0 means already emit-ready, nonzero means action required.

     To score candidates for a face:
     ./home/giovanni/projects/csgify propose surface face:N.M <STEP> --report surface.face-N-M.json

     Apply only high-confidence exact or near-exact candidates:
     ./home/giovanni/projects/csgify apply surface face:N.M <STEP> --candidate candidate:K --out tmp.i --log decisions.json --report report.after_surface.json

     If visual review is needed, ask me to run:
     ./home/giovanni/projects/csgify view <STEP> --review-surface-replacement candidate:K --select-face face:N.M

  6. Decompose non-convex solids:
     For solids or pieces requiring decomposition:
     ./home/giovanni/projects/csgify propose decompose solid:N <STEP> --report decomp.solid-N.json

     Apply high-confidence decomposition candidates only:
     ./home/giovanni/projects/csgify apply decompose solid:N <STEP> --candidate candidate:K --out tmp.i --log decisions.json --report report.after_decomp.json

     If ambiguous, ask me to review:
     ./home/giovanni/projects/csgify view <STEP> --review-decomp candidate:K --select solid:N

  7. Use Lua if multiple accepted operations are needed in one session. Prefer a script over repeating shell commands when applying many changes, so all operations
  happen on the same loaded document. The Lua API mirrors the CLI: `s:remove_solid`, `s:propose_decomposition`, `s:apply_decomposition_candidate`,
  `s:propose_surface_replacement`, `s:apply_surface_replacement_candidate`, `s:propose_hole_fills`, `s:fill_hole`. See docs/AGENT_WORKFLOW.md.

  8. After simplification, export simplified STEP for visual review:
     ./home/giovanni/projects/csgify simplify <STEP> <one-accepted-action> --out tmp.i --out-step simplified.step --report report.final.json --log decisions.json

     `simplify` accepts ONE action per invocation. For multi-action sequences, prefer a
     Lua script (step 7) so all ops run against the same loaded document.

  9. Compare volume:
     ./home/giovanni/projects/csgify volume <STEP> --compare simplified.step --rel-tol 1e-6

     If volume delta is larger than expected, stop and explain which operation caused the largest change.

  10. Convert to MCNP/CSG:
     Run the final csgify emit command, producing:
     output.i

  11. Compact the decision log into a replayable recipe so the same simplification can be
  re-run against a revised STEP without redoing analysis:
     ./home/giovanni/projects/csgify compact decisions.json --out recipe.lua
     ./home/giovanni/projects/csgify replay recipe.lua --step revised.step --out output.i --report replay.json

  12. Final response must include:
     - commands run
     - decisions applied
     - components removed
     - holes filled
     - surfaces replaced
     - solids decomposed
     - volume comparison
     - remaining warnings/unsupported geometry (use `explain` per remaining target)
     - output file path
     - decision log path
     - recipe path (if compacted)

  Important:
  If you are unsure whether a component is irrelevant for neutronics, ask me. If a spline/unsupported surface replacement is not high confidence, ask me. If
  decomposition has several similar candidates, ask me. Use the viewer commands to involve me visually.
