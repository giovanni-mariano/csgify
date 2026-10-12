-- lua_api: exercises the fixes to the Lua binding (Fix #1..#9).
--
-- This script is intentionally long: each block tests one fix in
-- isolation so a regression points at a specific area.

local function fail(msg) error("lua_api: " .. msg, 0) end

local fixtures = arg[1]
local outdir   = arg[2]

do
    local operations = csgify.operations()
    if type(operations) ~= "table" or #operations < 10 then
        fail("operation registry is not exposed to Lua")
    end
    local seen = {}
    for _, operation in ipairs(operations) do
        if seen[operation.name] then fail("duplicate operation: " .. operation.name) end
        seen[operation.name] = true
        if operation.version ~= 1 then
            fail("unexpected operation version for " .. operation.name)
        end
    end
    if not seen.replace_solid_with_bbox or not seen.undo_last_mutation then
        fail("operation registry is incomplete")
    end
end

----------------------------------------------------------------------
-- Phase 5: typed fluid material and contained-medium proposal bindings.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/box.step")
    local defined = s:define_material(doc, {
        id = "material:7", phase = "liquid", density = 0.997
    })
    if type(defined) ~= "table" then
        fail("define_material should return a report table")
    end
    local proposal = s:propose_contained_medium(doc, {
        id = "medium:not-a-pipe",
        material_id = "material:7",
        host_component_ids = { "inst:missing" },
        ports = {
            { port_id = "port:1", disposition = "cap_here" },
            { port_id = "port:2", disposition = "cap_here" },
        }
    })
    if type(proposal) ~= "table" or proposal.ok ~= false
       or type(proposal.diagnostics) ~= "table" then
        fail("contained-medium proposal binding returned an invalid shape")
    end
    local staged = s:stage_contained_medium_request(doc, {
        id = "medium:pending",
        material_id = "material:7",
        host_component_ids = { "inst:missing" },
        ports = {
            { port_id = "port:1", disposition = "unresolved" },
        }
    })
    if type(staged) ~= "table" then
        fail("stage_contained_medium_request should return a report table")
    end
end

----------------------------------------------------------------------
-- Batch removal is one atomic revision and one undo restores the batch.
-- A bad target must leave both the geometry and revision untouched.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/four_solids.step")

    local rejected = s:remove_solids({
        doc .. "/solid:2",
        doc .. "/solid:999",
        doc .. "/solid:3",
    })
    if rejected then fail("invalid removal batch unexpectedly succeeded") end
    if s:document_revision(doc) ~= 0 then
        fail("invalid removal batch changed the document revision")
    end
    local unchanged = s:analyze_table(doc)
    if unchanged.solids_seen ~= 4 then
        fail("invalid removal batch partially changed the model")
    end

    local removed, err = s:remove_solids({
        doc .. "/solid:2",
        doc .. "/solid:3",
        doc .. "/solid:2", -- duplicate IDs are idempotent within a batch
    })
    if not removed then fail("batch removal failed: " .. tostring(err)) end
    if removed.document_revision ~= 1 or removed.solids_seen ~= 2 then
        fail("batch removal did not commit exactly one two-solid revision")
    end

    local restored, undo_err = s:undo_last_mutation(doc)
    if not restored then fail("batch removal undo failed: " .. tostring(undo_err)) end
    if restored.document_revision ~= 2 or restored.solids_seen ~= 4 then
        fail("one undo did not restore the complete removal batch")
    end
end

do
    local s = csgify.session()
    local first = s:load_step(fixtures .. "/box.step")
    local second = s:load_step(fixtures .. "/box.step")
    local mixed = s:remove_solids({
        first .. "/solid:1",
        second .. "/solid:1",
    })
    if mixed then fail("mixed-document removal batch unexpectedly succeeded") end
    if s:document_revision(first) ~= 0 or s:document_revision(second) ~= 0 then
        fail("mixed-document removal batch changed a document revision")
    end
end

----------------------------------------------------------------------
-- Consecutive removal decisions stay batched in generated Lua recipes and
-- machine replay accepts the individual stable-target records atomically.
----------------------------------------------------------------------
do
    local log_path = outdir .. "/batch_remove_decisions.json"
    local recipe_path = outdir .. "/batch_remove_recipe.lua"
    local recipe_json = outdir .. "/batch_remove_recipe.json"
    local replay_out = outdir .. "/batch_remove_replay.i"
    local replay_report = outdir .. "/batch_remove_replay.json"
    os.remove(log_path)
    os.remove(recipe_path)
    os.remove(recipe_json)
    os.remove(replay_out)
    os.remove(replay_report)

    local s = csgify.session()
    s:attach_decision_log(log_path, fixtures .. "/four_solids.step")
    local doc = s:load_step(fixtures .. "/four_solids.step")
    local removed = s:remove_solids({
        doc .. "/solid:2",
        doc .. "/solid:3",
    })
    if not removed then fail("logged batch removal failed") end
    s = nil
    collectgarbage("collect")

    local compacted, compact_err = csgify.compact(log_path, recipe_path)
    if not compacted then
        fail("batch removal compact failed: " .. tostring(compact_err))
    end
    local recipe = io.open(recipe_path, "r")
    if not recipe then fail("batch removal recipe was not written") end
    local recipe_body = recipe:read("*a")
    recipe:close()
    if not recipe_body:find("s:remove_solids({", 1, true) then
        fail("generated recipe did not preserve batch removal")
    end
    if recipe_body:find("s:remove_solid(", 1, true) then
        fail("generated recipe regressed to sequential removal")
    end

    local replayed, replay_err = csgify.replay(
        recipe_json,
        fixtures .. "/four_solids.step",
        replay_out,
        replay_report)
    if not replayed then
        fail("machine replay of batch removal failed: " .. tostring(replay_err))
    end
    local deck = io.open(replay_out, "r")
    if not deck then fail("batch removal replay did not emit an output deck") end
    deck:close()
end

----------------------------------------------------------------------
-- Safety options are strict: a misspelled tolerance must not silently use
-- the default value.
----------------------------------------------------------------------
do
    local ok, err = pcall(function()
        csgify.session({ linear_tolerence = 1e-3 })
    end)
    if ok then fail("unknown conversion option was silently accepted") end
    if not tostring(err):find("unknown conversion option", 1, true) then
        fail("unexpected unknown-option error: " .. tostring(err))
    end
end

-- Batch approximation must never silently mean "unbounded".
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/spline_face.step")
    local ok, err = pcall(function() s:auto_fit_splines(doc) end)
    if ok then fail("auto_fit_splines accepted a missing deviation budget") end
    if not tostring(err):find("explicit max_deviation", 1, true) then
        fail("unexpected unbounded auto-fit error: " .. tostring(err))
    end
end

do
    local s = csgify.session({ decomposition_relative_volume_tolerance = -1 })
    local doc = s:load_step(fixtures .. "/l_shape.step")
    local result, report = s:decompose_planar(doc .. "/solid:1")
    if result then fail("invalid decomposition tolerance was accepted") end
    if s:document_revision(doc) ~= 0 then
        fail("rejected decomposition advanced the revision")
    end
    local found = false
    for _, diagnostic in ipairs(report.diagnostics or {}) do
        if diagnostic.code == "invalid_decomposition_volume_tolerance" then
            found = true
        end
    end
    if not found then fail("missing invalid decomposition tolerance diagnostic") end
end

----------------------------------------------------------------------
-- Atomic revisions: mutation advances once, undo restores geometry as a
-- new revision, and the revision counter is exposed consistently.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/box.step")
    if s:document_revision(doc) ~= 0 then
        fail("new document revision must be zero")
    end
    local removed, err = s:remove_solid(doc .. "/solid:1")
    if not removed then fail("revision remove failed: " .. tostring(err)) end
    if s:document_revision(doc) ~= 1 then
        fail("successful removal did not create revision 1")
    end
    local restored, undo_err = s:undo_last_mutation(doc)
    if not restored then fail("undo failed: " .. tostring(undo_err)) end
    if restored.document_revision ~= 2 or restored.solids_seen ~= 1 then
        fail("undo did not restore box as revision 2")
    end
end

----------------------------------------------------------------------
-- Fix #1: analyze_table on a model with errors must NOT return nil.
-- Before the fix, any spline / non-convex / missing-emitter STEP
-- caused the documented `if not report then error(err) end` pattern
-- to fail.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/spline_face.step")
    local report, err = s:analyze_table(doc)
    if not report then
        fail("analyze_table returned nil on spline_face.step")
    end
    if report.ok then
        fail("analyze_table on spline_face.step should set report.ok=false")
    end
    if #report.diagnostics == 0 then
        fail("expected diagnostics on spline_face.step")
    end
end

----------------------------------------------------------------------
-- Fix #1: replace_face_with_fit must return a non-nil report when
-- the targeted face replacement applied, even if sibling faces still
-- have spline errors.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/spline_face.step")
    local report, err = s:replace_face_with_fit(doc .. "/face:1.1",
                                                  "cylinder", 1e-6)
    if not report then
        fail("replace_face_with_fit returned nil for face:1.1")
    end
    -- The op applied; but the model still has spline faces 1.2/1.3.
    if report.ok then
        fail("expected report.ok=false (other spline faces remain)")
    end
end

----------------------------------------------------------------------
-- Fix #2: attach_decision_log opens a log; subsequent ops record.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local log_path = outdir .. "/lua_api_decisions.json"
    -- Ensure stale logs are gone before this run.
    os.remove(log_path)
    local ok = s:attach_decision_log(log_path)
    if not ok then fail("attach_decision_log returned false") end

    local doc = s:load_step(fixtures .. "/four_solids.step")
    local report, err = s:remove_solid(doc .. "/solid:2")
    if not report then fail("remove_solid failed: " .. tostring(err)) end

    -- The log file must exist and contain a record. We don't enforce
    -- a strict schema here; the decision-log schema is tested in its
    -- own smoke test. Just check the file was written.
    local f = io.open(log_path, "r")
    if not f then fail("decision log was not written to " .. log_path) end
    local body = f:read("*a")
    f:close()
    if #body == 0 then fail("decision log is empty") end
    if not body:find("remove_solid") then
        fail("decision log does not record remove_solid")
    end
end

----------------------------------------------------------------------
-- Fix #3: csgify.session({ ... }) and s:load_step(path, { ... })
-- accept ConversionOptions tables.
----------------------------------------------------------------------
do
    -- Session-level options (units assertion only; no rescale).
    local s = csgify.session({ units = "in" })
    local doc = s:load_step(fixtures .. "/box.step")
    local report = s:analyze_table(doc)
    if report.units.length_unit ~= "in" then
        fail("expected units.length_unit='in', got "
             .. tostring(report.units.length_unit))
    end
end

do
    -- Per-load options override session defaults; also exercise the
    -- M2 normalize_units path (rescales geometry).
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/box.step",
                            { normalize_units = "cm" })
    local report = s:analyze_table(doc)
    if report.units.length_unit ~= "cm" then
        fail("expected normalized unit cm, got "
             .. tostring(report.units.length_unit))
    end
    -- box.step is a 10x5x2 mm box = 100 mm^3 = 0.1 cm^3.
    if math.abs(report.total_volume - 0.1) > 1e-6 then
        fail("expected total_volume ~ 0.1 cm^3, got "
             .. tostring(report.total_volume))
    end
end

----------------------------------------------------------------------
-- Fix #4: push_report_table exposes M2 fields and the structured
-- replacement_candidates / decomposed_pieces blocks.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/box.step")
    local report = s:analyze_table(doc)
    if type(report.units) ~= "table"
       or type(report.units.length_unit) ~= "string" then
        fail("report.units missing length_unit")
    end
    if type(report.performance) ~= "table"
       or type(report.performance.analyze_ms) ~= "number" then
        fail("report.performance missing analyze_ms")
    end
    if type(report.decomposed_pieces) ~= "table" then
        fail("report.decomposed_pieces missing")
    end
    local solid = report.solids[1]
    if type(solid.replacement_candidates) ~= "table" then
        fail("solid.replacement_candidates missing")
    end
    if type(solid.replacement_candidates.bbox) ~= "table" then
        fail("solid.replacement_candidates.bbox missing")
    end
    if type(solid.replacement_candidates.bbox.available) ~= "boolean" then
        fail("bbox.available must be boolean")
    end
    if type(solid.bbox_diagonal) ~= "number" then
        fail("solid.bbox_diagonal missing")
    end
end

----------------------------------------------------------------------
-- Fix #5: every mutating op returns a table (not a JSON string).
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/four_solids.step")
    local r = s:remove_solid(doc .. "/solid:2")
    if type(r) ~= "table" then
        fail("remove_solid should return a table, got " .. type(r))
    end
    if type(r.solids) ~= "table" then
        fail("table from remove_solid missing .solids")
    end
end

----------------------------------------------------------------------
-- Review fix: removed solids are no longer valid mutation targets.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/box.step")
    local removed = s:remove_solid(doc .. "/solid:1")
    if not removed then fail("initial remove_solid failed") end
    local replaced = s:replace_solid_with_bbox(doc .. "/solid:1")
    if replaced then
        fail("replace_solid_with_bbox succeeded on an already removed solid")
    end
    local report = s:analyze_table(doc)
    if report.solids_seen ~= 0 then
        fail("removed solid reappeared in analysis")
    end
end

----------------------------------------------------------------------
-- Review fix: scoped reports must scope edge/reference ids too.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/overlapping_boxes.step")
    local report = s:analyze_table(doc)
    local overlap = report.overlaps[1]
    if not overlap then fail("expected overlap in overlapping_boxes.step") end
    if overlap.solid_a_id ~= doc .. "/solid:1"
       or overlap.solid_b_id ~= doc .. "/solid:2" then
        fail("overlap ids were not document-scoped")
    end
end

do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/spline_face.step")
    local report = s:analyze_table(doc)
    local request = report.decision_requests[1]
    if not request then fail("expected spline decision request") end
    if not request.target_id:find("^" .. doc .. "/") then
        fail("decision request target_id was not document-scoped")
    end
    if not request.id:find(doc .. "/", 1, true) then
        fail("decision request id did not carry scoped target identity")
    end
end

----------------------------------------------------------------------
-- Fix #8: checked replace_solid returns a `check` sub-table with
-- metrics, so callers can inspect the volume guard outcome directly.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/four_solids.step")
    -- Tight rel-delta to ensure the check is consulted but still passes
    -- (the box -> bbox replacement is a no-op delta on an axis-aligned box).
    local r, err = s:replace_solid(doc .. "/solid:1", "bbox",
                                    { max_rel_delta = 1.0 })
    if not r then fail("replace_solid failed: " .. tostring(err)) end
    if type(r.check) ~= "table" then
        fail("checked replace_solid missing .check")
    end
    if type(r.check.metrics) ~= "table" then
        fail("r.check.metrics missing")
    end
    if type(r.check.metrics.relative_delta) ~= "number" then
        fail("r.check.metrics.relative_delta must be a number")
    end
end

----------------------------------------------------------------------
-- Fix #9: export_step writes a STEP file.
----------------------------------------------------------------------
do
    local s = csgify.session()
    local doc = s:load_step(fixtures .. "/four_solids.step")
    s:remove_solid(doc .. "/solid:2")  -- mutate first to exercise the path
    local step_path = outdir .. "/lua_api_export.step"
    os.remove(step_path)
    local r, err = s:export_step(doc, step_path)
    if not r then fail("export_step failed: " .. tostring(err)) end
    local f = io.open(step_path, "r")
    if not f then fail("export_step did not write " .. step_path) end
    local header = f:read(80)
    f:close()
    if not header or not header:find("ISO%-10303") then
        fail("export_step output is not a STEP file (header: "
             .. tostring(header) .. ")")
    end
end

print("lua_api: OK")
