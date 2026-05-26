-- lua_coverage: confirms Lua bindings cover the CLI verbs added
-- in the coverage audit:
--   s:explain               (CLI: explain)
--   cad2csg.compact         (CLI: compact)
--   cad2csg.replay          (CLI: replay)
--   s:write_decision_pack   (CLI: decision-pack)
--   s:preprocess_safe_all   (CLI: preprocess --safe-all)

local function fail(msg) error("lua_coverage: " .. msg, 0) end

local fixtures = arg[1]
local outdir   = arg[2]

----------------------------------------------------------------------
-- s:explain
----------------------------------------------------------------------
do
    local s = cad2csg.session()
    local doc = s:load_step(fixtures .. "/spline_face.step")
    -- analyze first so the solid table is populated.
    s:analyze_table(doc)

    -- Solid target: should return kind=solid with the right blockers.
    local info = s:explain(doc .. "/solid:1")
    if type(info) ~= "table" then fail("explain returned " .. type(info)) end
    if info.kind ~= "solid" then fail("explain kind=" .. tostring(info.kind)) end
    if info.ready then fail("expected solid:1 not ready on spline_face.step") end
    if type(info.blocking_faces) ~= "table" or #info.blocking_faces == 0 then
        fail("explain missing blocking_faces")
    end

    -- Face target: should propose surface-replacement candidates.
    local face_info = s:explain(doc .. "/face:1.1")
    if face_info.kind ~= "face" then fail("face explain kind=" .. tostring(face_info.kind)) end
    if face_info.ready then fail("expected face:1.1 not ready") end
    if type(face_info.surface_replacement) ~= "table" then
        fail("face explain missing surface_replacement")
    end

    -- Unknown target: kind=unknown.
    local missing = s:explain("solid:99")
    if missing.kind ~= "unknown" then
        fail("expected kind=unknown for missing target, got " .. tostring(missing.kind))
    end
end

----------------------------------------------------------------------
-- cad2csg.compact + cad2csg.replay
----------------------------------------------------------------------
do
    local log_path = outdir .. "/coverage_decisions.json"
    os.remove(log_path)

    -- Record a small log: load + remove_solid + emit. Wrap in a do/end
    -- block so the session is collectable (and the log file flushed)
    -- before we read it back.
    do
        local s = cad2csg.session()
        s:attach_decision_log(log_path)
        local doc = s:load_step(fixtures .. "/four_solids.step")
        s:remove_solid(doc .. "/solid:2")
        s:emit(doc, outdir .. "/coverage_first.i")
    end
    collectgarbage("collect")  -- force the DecisionLog destructor to fire

    -- Compact to a Lua recipe (text script callable via `cad2csg lua`).
    local recipe_path = outdir .. "/coverage_recipe.lua"
    os.remove(recipe_path)
    local ok, err = cad2csg.compact(log_path, recipe_path)
    if not ok then fail("cad2csg.compact failed: " .. tostring(err)) end
    local f = io.open(recipe_path, "r")
    if not f then fail("recipe not written") end
    local body = f:read("*a"); f:close()
    if not body:find("cad2csg.session") then
        fail("compacted recipe doesn't look like a Lua script")
    end

    -- Replay against the SAME STEP via the JSON-recipe entry-point. The
    -- compactor produces a .lua recipe; `replay_recipe` (CLI: replay)
    -- consumes a JSON recipe with soft-match support. Hand-craft a
    -- minimal one here so we exercise the Lua binding.
    local step = fixtures .. "/box.step"
    local json_recipe = outdir .. "/coverage_recipe.json"
    local replay_out  = outdir .. "/coverage_replay.i"
    local replay_report = outdir .. "/coverage_replay.json"
    os.remove(json_recipe); os.remove(replay_out); os.remove(replay_report)
    local json = string.format([[{
  "manifest": {"step_path":"%s","step_digest":"","cad2csg_version":"unversioned","occt_version":"8.0.0"},
  "ops": [
    {"op":"load_step","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"step_path":"%s"}},
    {"op":"replace_solid_with_bbox","target":{"kind":"solid","shape_id":"solid:1","component_entry":"0:1:1:1","fingerprint":{"volume":100.0,"surface_area":160.0,"bbox_diagonal":11.358,"face_count":6,"centroid":[5.0,2.5,1.0]}},"params":{}},
    {"op":"emit_mcnp","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"output_path":"%s"}}
  ]
}]], step, step, replay_out)
    local jf = io.open(json_recipe, "w")
    jf:write(json); jf:close()

    local replay_ok, replay_err = cad2csg.replay(
        json_recipe, step, replay_out, replay_report)
    if not replay_ok then fail("cad2csg.replay failed: " .. tostring(replay_err)) end
    local rf = io.open(replay_out, "r")
    if not rf then fail("replay did not write " .. replay_out) end
    rf:close()
end

----------------------------------------------------------------------
-- s:write_decision_pack
----------------------------------------------------------------------
do
    local s = cad2csg.session()
    local doc = s:load_step(fixtures .. "/spline_face.step")
    s:analyze_table(doc)  -- populate decision_requests

    local pack_dir = outdir .. "/coverage_pack"
    os.execute("rm -rf '" .. pack_dir .. "'")
    local result = s:write_decision_pack(doc, pack_dir)
    if type(result) ~= "table" then fail("write_decision_pack returned " .. type(result)) end
    -- spline_face.step has decision requests for unsupported_spline_face;
    -- packets_written should be > 0 OR ok=true with 0 packets (depending
    -- on which targets the pack writer chooses to emit). We just assert
    -- the result is well-formed.
    if type(result.ok) ~= "boolean" then fail("result.ok missing") end
    if type(result.packets_written) ~= "number" then
        fail("result.packets_written missing")
    end
end

----------------------------------------------------------------------
-- s:preprocess_safe_all
----------------------------------------------------------------------
do
    -- l_shape.step: one non-convex solid the safe BSP can resolve.
    local s = cad2csg.session()
    local doc = s:load_step(fixtures .. "/l_shape.step")
    local summary = s:preprocess_safe_all(doc)
    if type(summary) ~= "table" then fail("safe_all returned " .. type(summary)) end
    if not summary.ok then fail("safe_all failed: " .. tostring(summary.error)) end
    if summary.overlaps_aborted then fail("clean fixture should not abort on overlap") end
    if summary.attempted == 0 then fail("expected at least one BSP attempt") end
end

do
    -- overlapping_boxes.step: should abort the safe-all precheck.
    local s = cad2csg.session({ allow_overlaps = false })
    local doc = s:load_step(fixtures .. "/overlapping_boxes.step")
    local summary = s:preprocess_safe_all(doc, { allow_overlaps = false })
    if summary.ok then fail("safe_all should abort on overlapping_boxes.step") end
    if not summary.overlaps_aborted then
        fail("expected overlaps_aborted=true")
    end
end

print("lua_coverage: OK")
