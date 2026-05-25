-- lua_api: exercises the fixes to the Lua binding (Fix #1..#9).
--
-- This script is intentionally long: each block tests one fix in
-- isolation so a regression points at a specific area.

local function fail(msg) error("lua_api: " .. msg, 0) end

local fixtures = arg[1]
local outdir   = arg[2]

----------------------------------------------------------------------
-- Fix #1: analyze_table on a model with errors must NOT return nil.
-- Before the fix, any spline / non-convex / missing-emitter STEP
-- caused the documented `if not report then error(err) end` pattern
-- to fail.
----------------------------------------------------------------------
do
    local s = cad2csg.session()
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
    local s = cad2csg.session()
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
    local s = cad2csg.session()
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
-- Fix #3: cad2csg.session({ ... }) and s:load_step(path, { ... })
-- accept ConversionOptions tables.
----------------------------------------------------------------------
do
    -- Session-level options (units assertion only; no rescale).
    local s = cad2csg.session({ units = "in" })
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
    local s = cad2csg.session()
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
    local s = cad2csg.session()
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
    local s = cad2csg.session()
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
-- Fix #8: checked replace_solid returns a `check` sub-table with
-- metrics, so callers can inspect the volume guard outcome directly.
----------------------------------------------------------------------
do
    local s = cad2csg.session()
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
    local s = cad2csg.session()
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
