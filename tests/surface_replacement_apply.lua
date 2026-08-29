local s = csgify.session()
local doc = s:load_step(arg[1])

local function fail(msg)
    error(msg, 0)
end

local function best_high_confidence(face_id)
    local proposal = s:propose_surface_replacement(face_id)
    if not proposal.ok then
        fail("expected proposal for " .. face_id)
    end
    if proposal.best_candidate_id == "" then
        fail("expected best candidate for " .. face_id)
    end
    local best
    for _, candidate in ipairs(proposal.candidates) do
        if candidate.id == proposal.best_candidate_id then
            best = candidate
            break
        end
    end
    if not best then
        fail("best candidate not present for " .. face_id)
    end
    if not best.usable then
        fail("best candidate not usable for " .. face_id)
    end
    if best.confidence ~= "high" then
        fail("expected high confidence for " .. face_id .. ", got " .. best.confidence)
    end
    return best
end

for _, face_id in ipairs({
    doc .. "/face:1.1",
    doc .. "/face:1.2",
    doc .. "/face:1.3",
}) do
    local candidate = best_high_confidence(face_id)
    local report, err = s:apply_surface_replacement_candidate(face_id, candidate.id)
    if not report then
        fail(err)
    end
end

local report, err = s:analyze_table(doc)
if not report then
    fail(err)
end

for _, face in ipairs(report.faces) do
    if face.needs_surface_replacement then
        fail("surface still needs replacement: " .. face.id)
    end
end

local emitted, emit_err = s:emit(doc, arg[2])
if not emitted then
    fail(emit_err)
end
