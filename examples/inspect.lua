local s = cad2csg.session()
local doc = s:load_step(arg[1])
local report, err = s:analyze(doc)
if not report then
    error(err)
end
print(report)

