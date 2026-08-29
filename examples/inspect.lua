local s = csgify.session()
local doc = s:load_step(arg[1])
local report, err = s:analyze(doc)
if not report then
    error(err)
end
-- Session calls return a structured report table; `report.json` is the
-- canonical JSON form for downstream tooling.
print(report.json)
