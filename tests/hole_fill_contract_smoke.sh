#!/usr/bin/env bash
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "hole_fill_contract_smoke: $*" >&2
    exit 1
}

report="$outdir/hole_fills.json"
"$exe" hole-fill-suggest "$fixtures/box.step" \
    --host solid:1 \
    --max-radius 5 \
    --max-volume-fraction 0.001 \
    --report "$report" >/dev/null 2>&1 \
    || fail "hole-fill-suggest failed"

python3 - "$report" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["ok"] is True, data
assert isinstance(data["candidates"], list), data
assert isinstance(data["decision_requests"], list), data
assert isinstance(data["diagnostics"], list), data
assert data["candidates"] == [], data
assert data["diagnostics"][0]["code"] == "hole_fill_no_candidates", data
PY
if [ "$?" -ne 0 ]; then
    fail "hole-fill proposal JSON contract validation failed"
fi

stdout="$outdir/hole_fills_stdout.json"
"$exe" hole-fill-suggest "$fixtures/box.step" >"$stdout" 2>/dev/null \
    || fail "hole-fill-suggest stdout mode failed"
python3 - "$stdout" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["ok"] is True, data
assert "candidates" in data, data
assert "decision_requests" in data, data
assert "diagnostics" in data, data
PY
if [ "$?" -ne 0 ]; then
    fail "hole-fill stdout JSON contract validation failed"
fi

external_report="$outdir/external_cylinder.json"
"$exe" hole-fill-suggest "$fixtures/cylinder.step" \
    --host solid:1 \
    --max-radius 3 \
    --max-volume-fraction 1 \
    --report "$external_report" >/dev/null 2>&1 \
    || fail "hole-fill-suggest failed on external cylinder"
python3 - "$external_report" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["ok"] is True, data
assert data["candidates"] == [], data
assert data["diagnostics"][0]["code"] == "hole_fill_no_candidates", data
PY
if [ "$?" -ne 0 ]; then
    fail "external cylinder should not be proposed as a hole"
fi

bolt_report="$outdir/bolt_hole_fills.json"
"$exe" hole-fill-suggest "$fixtures/bolt_hole.step" \
    --host solid:1 \
    --near-removed solid:2 \
    --max-radius 2 \
    --max-volume-fraction 1 \
    --report "$bolt_report" >/dev/null 2>&1 \
    || fail "hole-fill-suggest failed on bolt_hole"

python3 - "$bolt_report" <<'PY'
import json
import math
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["ok"] is True, data
assert len(data["candidates"]) == 1, data
c = data["candidates"][0]
assert c["id"] == "hole-fill:1", c
assert c["host_solid_id"] == "solid:1", c
assert c["seed_removed_solid_ids"] == ["solid:2"], c
assert c["kind"] == "cylindrical_through", c
assert c["confidence"] == "high", c
assert c["suggested_action"] == "fill_hole", c
assert math.isclose(c["radius"], 1.0, rel_tol=0.0, abs_tol=1e-8), c
assert math.isclose(c["length"], 4.0, rel_tol=0.0, abs_tol=1e-5), c
assert len(data["decision_requests"]) == 1, data
assert data["decision_requests"][0]["kind"] == "hole_fill", data
assert data["decision_requests"][0]["recommended_action"] == "fill_hole", data
PY
if [ "$?" -ne 0 ]; then
    fail "bolt-hole proposal JSON validation failed"
fi

exit 0
