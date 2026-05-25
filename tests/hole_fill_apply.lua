local s = cad2csg.session()
local doc = s:load_step(arg[1])

local function expect_fill_error(code, fn)
    local ok, err_report = fn()
    if ok then
        error("expected fill_hole to fail with " .. code)
    end
    if type(err_report) ~= "table"
        or type(err_report.diagnostics) ~= "table"
        or #err_report.diagnostics == 0 then
        error("expected diagnostic table for " .. code)
    end
    if err_report.diagnostics[1].code ~= code then
        error("expected " .. code .. ", got "
              .. tostring(err_report.diagnostics[1].code))
    end
end

do
    local check = cad2csg.session()
    local check_doc = check:load_step(arg[1])
    expect_fill_error("hole_fill_candidate_not_found", function()
        return check:fill_hole(check_doc, "hole-fill:1")
    end)
end

do
    local check = cad2csg.session()
    local check_doc = check:load_step(arg[1])
    local check_holes = check:propose_hole_fills(check_doc, {
        host = check_doc .. "/solid:1",
        near_removed = check_doc .. "/solid:2",
        max_radius = 2.0,
        max_volume_fraction = 1.0
    })
    if #check_holes.candidates ~= 1 then
        error("expected one pre-removal hole-fill candidate")
    end
    expect_fill_error("hole_fill_source_still_active", function()
        return check:fill_hole(check_doc, check_holes.candidates[1].id)
    end)
end

do
    local check = cad2csg.session()
    local check_doc = check:load_step(arg[1])
    local check_holes = check:propose_hole_fills(check_doc, {
        host = check_doc .. "/solid:1",
        near_removed = check_doc .. "/solid:2",
        max_radius = 2.0,
        max_volume_fraction = 1.0
    })
    check:remove_solid(check_doc .. "/solid:1")
    expect_fill_error("hole_fill_host_inactive", function()
        return check:fill_hole(check_doc, check_holes.candidates[1].id)
    end)
end

-- remove_solid may return a non-ok report when the remaining host still needs
-- preprocessing, but the suppression itself is applied. The fill operation
-- below is exactly the repair that makes this simple fixture whole again.
s:remove_solid(doc .. "/solid:2")

local holes = s:propose_hole_fills(doc, {
    host = doc .. "/solid:1",
    near_removed = doc .. "/solid:2",
    max_radius = 2.0,
    max_volume_fraction = 1.0
})
if not holes.ok then
    error("hole-fill proposal failed")
end
if #holes.candidates ~= 1 then
    error("expected one hole-fill candidate, got " .. tostring(#holes.candidates))
end

report, err = s:fill_hole(doc, holes.candidates[1].id)
if not report then
    error(err)
end
local saw_applied_diag = false
for _, diagnostic in ipairs(report.diagnostics) do
    if diagnostic.code == "hole_fill_applied" then
        saw_applied_diag = true
    end
end
if not saw_applied_diag then
    error("expected hole_fill_applied diagnostic")
end
if report.solids_seen ~= 2 then
    error("expected host plus fill body after removing bolt")
end
if math.abs(report.total_volume - 400.0) > 1e-4 then
    error("expected filled block volume near 400, got " .. tostring(report.total_volume))
end

local saw_fill = false
for _, solid in ipairs(report.solids) do
    if solid.label == "fill_hole_from_solid:1_after_removing_solid:2" then
        saw_fill = true
    end
end
if not saw_fill then
    error("expected synthetic fill body in analysis report")
end

local emitted, emit_err = s:emit(doc, arg[2])
if not emitted then
    error(emit_err)
end
report, err = s:report_table()
if not report then
    error(err)
end
if report.solids_converted ~= 2 then
    error("expected host plus fill body to emit, got " .. tostring(report.solids_converted))
end
