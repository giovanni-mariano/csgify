local s = cad2csg.session()
local doc = s:load_step(arg[1])

local report, err = s:auto_fit_splines(doc, 1e-6)
if not report then
    error(err)
end

report, err = s:emit(doc, arg[2])
if not report then
    error(err)
end
print(report)

