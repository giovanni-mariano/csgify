local s = cad2csg.session()
local original = s:load_step(arg[1])
local work = s:load_step(arg[1])

local report, err = s:analyze_table(work)
if not report then
    error(err)
end
if #report.solids ~= 1 then
    error("expected one solid in report.solids")
end
if #report.faces == 0 then
    error("expected report.faces to be populated")
end
if type(report.diagnostics) ~= "table" then
    error("expected report.diagnostics table")
end
if type(report.assembly) ~= "table" or #report.assembly == 0 then
    error("expected report.assembly to be populated")
end
if report.solids[1].id ~= work .. "/solid:1" then
    error("unexpected scoped solid id: " .. tostring(report.solids[1].id))
end
if report.assembly[1].shape_id ~= work .. "/solid:1" then
    error("unexpected assembly shape id: " .. tostring(report.assembly[1].shape_id))
end

report, err = s:replace_solid(work .. "/solid:1", "bbox", {
    max_rel_delta = 1.0
})
if not report then
    error(err)
end

local cmp = s:compare_volumes(original, work, { rel_tol = 1e-6 })
if not cmp.within_tolerance then
    error("expected bbox replacement of box fixture to preserve volume")
end

local ok, write_err = s:write_report(arg[3])
if not ok then
    error(write_err)
end

report, err = s:emit(work, arg[2])
if not report then
    error(err)
end
