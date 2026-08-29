-- lua_api: exercises the fixes to the Lua binding (Fix #1..#9).
--
-- This script is intentionally long: each block tests one fix in
-- isolation so a regression points at a specific area.

local function fail(msg) error("lua_api: " .. msg, 0) end

local fixtures = arg[1]
local outdir   = arg[2]

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
