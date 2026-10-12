#!/usr/bin/env bash
set -u

exe="$1"
generator="$2"
outdir="$3"
mkdir -p "$outdir"
fail() { echo "mcnp_domain_cli_smoke: $*" >&2; exit 1; }

"$generator" "$outdir" || fail "could not generate modeled-domain fixtures"
rm -f "$outdir/cli-covered.i" "$outdir/cli-gap.i"

"$exe" simplify "$outdir/covered.step" --replace-solid-bbox solid:2 \
  --out "$outdir/cli-covered.i" --mcnp-domain solid:1 \
  --require-domain-coverage --report "$outdir/cli-covered.json" \
  >/dev/null 2>"$outdir/cli-covered.err" \
  || fail "exactly covered modeled domain was rejected"
[[ -s "$outdir/cli-covered.i" ]] || fail "certified domain wrote no MCNP"
grep -q '"coverage_status": "certified"' "$outdir/cli-covered.json" \
  || fail "certified coverage status missing"
grep -q '"safe_to_emit": true' "$outdir/cli-covered.json" \
  || fail "safe-to-emit certificate missing"

set +e
"$exe" simplify "$outdir/gap.step" --replace-solid-bbox solid:2 \
  --out "$outdir/cli-gap.i" --mcnp-domain solid:1 \
  --require-domain-coverage --report "$outdir/cli-gap.json" \
  >/dev/null 2>"$outdir/cli-gap.err"
gap_rc=$?
set -e
[[ "$gap_rc" == "1" ]] || fail "unclassified gap did not block emission"
[[ ! -e "$outdir/cli-gap.i" ]] || fail "gap left an MCNP output behind"
grep -q '"coverage_status": "gap"' "$outdir/cli-gap.json" \
  || fail "gap coverage status missing"
grep -q '"failure_code": "coverage_gap"' "$outdir/cli-gap.json" \
  || fail "gap failure code missing"

echo "mcnp_domain_cli_smoke: OK"
