#!/usr/bin/env python3
"""Run reproducible, bounded csgify decomposition benchmarks.

Large/licensed STEP files stay outside the repository and are selected by a
manifest entry plus SHA-256.  Each repetition gets an isolated output folder
and retains stdout, stderr, progress, decision log, STEP, MCNP, report, GNU
time evidence, and one normalized result.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time
from typing import Any


HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_gnu_time(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {}
    if not path.exists():
        return result
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("Maximum resident set size (kbytes):"):
            result["peak_rss_kib"] = int(line.rsplit(":", 1)[1].strip())
        elif line.startswith("User time (seconds):"):
            result["user_seconds"] = float(line.rsplit(":", 1)[1].strip())
        elif line.startswith("System time (seconds):"):
            result["system_seconds"] = float(line.rsplit(":", 1)[1].strip())
        elif line.startswith("Percent of CPU this job got:"):
            result["cpu_percent"] = line.rsplit(":", 1)[1].strip()
    return result


def load_json(path: Path) -> Any:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {"parse_error": str(exc)}


def resolve_input(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = REPO / path
    return path.resolve()


def git_revision() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=REPO, text=True,
            stderr=subprocess.DEVNULL, timeout=5).strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def run_once(exe: Path, name: str, case: dict[str, Any], source: Path,
             output: Path, repetition: int) -> dict[str, Any]:
    run_dir = output / f"run-{repetition:02d}"
    run_dir.mkdir(parents=True, exist_ok=False)
    deck = run_dir / "output.i"
    step = run_dir / "output.step"
    report_path = run_dir / "report.json"
    progress = run_dir / "progress.jsonl"
    decisions = run_dir / "decisions.json"
    stdout_path = run_dir / "stdout.txt"
    stderr_path = run_dir / "stderr.txt"
    time_path = run_dir / "time.txt"
    command = [
        "/usr/bin/time", "-v", "-o", str(time_path),
        str(exe), "decompose-apply", str(source),
        "--out", str(deck), "--out-step", str(step),
        "--report", str(report_path), "--progress-json", str(progress),
        "--log", str(decisions), *case.get("arguments", []),
    ]
    timeout = int(case.get("timeout_seconds", 600))
    started = time.monotonic()
    timed_out = False
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            command, cwd=REPO, stdout=stdout, stderr=stderr,
            start_new_session=True)
        try:
            return_code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                return_code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                return_code = process.wait()
    wall_seconds = time.monotonic() - started
    report = load_json(report_path)
    progress_events = []
    if progress.exists():
        for line in progress.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                progress_events.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    result = {
        "schema": "csgify.decomposition-benchmark-result",
        "schema_version": 1,
        "case": name,
        "repetition": repetition,
        "input": str(source),
        "input_sha256": file_sha256(source),
        "command": command,
        "timeout_seconds": timeout,
        "timed_out": timed_out,
        "return_code": return_code,
        "wall_seconds": wall_seconds,
        "gnu_time": parse_gnu_time(time_path),
        "artifacts": {
            "deck_written": deck.is_file() and deck.stat().st_size > 0,
            "step_written": step.is_file() and step.stat().st_size > 0,
            "report_written": report_path.is_file() and report_path.stat().st_size > 0,
            "progress_events": len(progress_events),
        },
        "terminal_progress": progress_events[-1] if progress_events else None,
        "report_ok": report.get("ok") if isinstance(report, dict) else None,
        "summary": report.get("summary") if isinstance(report, dict) else None,
        "performance": report.get("performance") if isinstance(report, dict) else None,
    }
    (run_dir / "result.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", default="triangle")
    parser.add_argument("--exe", type=Path, default=REPO / "build" / "csgify")
    parser.add_argument("--manifest", type=Path, default=HERE / "cases.json")
    parser.add_argument("--input", type=Path,
                        help="override the manifest input (digest is still checked)")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    cases = manifest.get("cases", {})
    if args.case not in cases:
        parser.error(f"unknown case {args.case!r}; choose from {', '.join(sorted(cases))}")
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    case = cases[args.case]
    source = (args.input.expanduser().resolve() if args.input
              else resolve_input(case["input"]))
    exe = args.exe.expanduser().resolve()
    if not source.is_file():
        parser.error(f"input STEP does not exist: {source}")
    if not exe.is_file():
        parser.error(f"csgify executable does not exist: {exe}")
    expected_digest = case.get("sha256")
    actual_digest = file_sha256(source)
    if expected_digest and actual_digest != expected_digest:
        parser.error(
            f"input digest mismatch: expected {expected_digest}, got {actual_digest}")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    output = (args.output.expanduser().resolve() if args.output
              else HERE / "results" / f"{args.case}-{stamp}")
    output.mkdir(parents=True, exist_ok=False)
    metadata = {
        "schema": "csgify.decomposition-benchmark-run",
        "schema_version": 1,
        "case": args.case,
        "description": case.get("description", ""),
        "input": str(source),
        "input_sha256": actual_digest,
        "executable": str(exe),
        "executable_sha256": file_sha256(exe),
        "git_revision": git_revision(),
        "platform": platform.platform(),
        "python": sys.version,
        "repeat": args.repeat,
    }
    (output / "run.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    results = [run_once(exe, args.case, case, source, output, repetition)
               for repetition in range(1, args.repeat + 1)]
    summary = {
        **metadata,
        "results": results,
        "successful_runs": sum(
            result["return_code"] == 0 and not result["timed_out"]
            for result in results),
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(output)
    return 0 if summary["successful_runs"] == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
