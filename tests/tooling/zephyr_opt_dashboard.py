#!/usr/bin/env python3
"""Collect and render DWM3001CDK Zephyr optimization evidence.

The collector is deliberately read-mostly.  It consumes the repository's
canonical cdk-size report, sanitized latency lines, and an optional explicitly
shaped runtime snapshot.  It never treats a missing tool, build, or hardware run
as a zero.  Generated data lives under the ignored internal/ tree by default.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import html
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
from typing import Any, Iterable


SCHEMA_VERSION = 1
SIZE_IMAGE = "dwm3001cdk-lock"
NCS_VERSION = "v3.3.0"
PHASES = (
    "connect", "spsm", "ver", "l2cap", "op05", "auth0", "a0rsp", "auth1",
    "exch", "apc", "irs", "m1", "m2", "m3", "m4rx", "m4", "range",
    "trusted", "near", "bolt",
)
CONFIG_ALLOWLIST = (
    "CONFIG_LTO",
    "CONFIG_ISR_TABLES_LOCAL_DECLARATION",
    "CONFIG_SEGGER_RTT_BUFFER_SIZE_UP",
    "CONFIG_INIT_STACKS",
    "CONFIG_MCUMGR",
    "CONFIG_DEBUG",
    "CONFIG_LOG",
    "CONFIG_LOG_DEFAULT_LEVEL",
    "CONFIG_SPEED_OPTIMIZATIONS",
    "CONFIG_SIZE_OPTIMIZATIONS",
    "CONFIG_NO_OPTIMIZATIONS",
    "CONFIG_OPENTHREAD_DEBUG",
    "CONFIG_ULTRAWIDELOCK_MATTER_BLE",
    "CONFIG_ULTRAWIDELOCK_BENCH",
    "CONFIG_ULTRAWIDELOCK_LAT_TRACE",
    "CONFIG_ULTRAWIDELOCK_RANGE_GATE_STRICT",
    "CONFIG_DW3000_SPI_METRICS",
    "CONFIG_STACK_USAGE",
    "CONFIG_TIMING_FUNCTIONS",
    "CONFIG_THREAD_MONITOR",
    "CONFIG_THREAD_STACK_INFO",
    "CONFIG_THREAD_RUNTIME_STATS",
)
STATUS_VALUES = {"measured", "unavailable", "failed", "not-run"}
LABEL_RE = re.compile(r"^[A-Za-z0-9_.+ /:(),-]{1,160}$")
MAC_RE = re.compile(r"(?i)(?:\b[0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b")
LONG_HEX_RE = re.compile(r"(?i)\b[0-9a-f]{32,}\b")
HOME_PATH_RE = re.compile(r"(?:(?:/Users|/home)/)[^/\s]+")
SECRET_WORD_RE = re.compile(
    r"(?i)\b(?:password|passwd|token|secret|private[ _-]?key|credential[ _-]?id|network[ _-]?key)\b"
)
LATENCY_MARKER = "ultrawidelock-lat:"
LATENCY_LINE_RE = re.compile(r"^ultrawidelock-lat:(?P<body>.*) ms$")
LATENCY_TOKEN_RE = re.compile(r"^(?P<phase>[a-z0-9]+)(?:\+(?P<value>\d+)|(?P<missing>-))$")


class DashboardError(RuntimeError):
    """A user-actionable collector or validation error."""


def repo_default() -> Path:
    return Path(__file__).resolve().parents[2]


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def status_record(status: str, detail: str, artifact: str | None = None) -> dict[str, Any]:
    if status not in STATUS_VALUES:
        raise DashboardError(f"invalid evidence status {status!r}")
    out: dict[str, Any] = {"status": status, "detail": detail}
    if artifact:
        out["artifact"] = artifact
    return out


def safe_label(value: Any, field: str, *, allow_empty: bool = False) -> str:
    if value is None and allow_empty:
        return ""
    if not isinstance(value, str):
        raise DashboardError(f"{field} must be a string")
    value = value.strip()
    if not value and allow_empty:
        return ""
    if not LABEL_RE.fullmatch(value):
        raise DashboardError(f"{field} contains unsupported characters or is too long")
    if MAC_RE.search(value) or LONG_HEX_RE.search(value) or SECRET_WORD_RE.search(value):
        raise DashboardError(f"{field} looks sensitive and was refused")
    return value


def checked_int(value: Any, field: str, *, minimum: int = 0, maximum: int = 2**63 - 1) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise DashboardError(f"{field} must be an integer")
    if value < minimum or value > maximum:
        raise DashboardError(f"{field} is outside {minimum}..{maximum}")
    return value


def checked_float(value: Any, field: str, *, minimum: float = 0.0, maximum: float = 100.0) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        raise DashboardError(f"{field} must be a finite number")
    value = float(value)
    if value < minimum or value > maximum:
        raise DashboardError(f"{field} is outside {minimum}..{maximum}")
    return value


def sanitize_text(value: str, repo: Path | None = None) -> str:
    """Remove machine paths and secret-shaped fragments from persisted text."""
    if repo:
        value = value.replace(str(repo.resolve()), "<repo>")
    value = HOME_PATH_RE.sub("/<user>", value)
    value = re.sub(
        r"(?i)(?:apps/dwm3001cdk-lock|firmware)/keys/[^\s'\"]+",
        "<redacted-key-path>",
        value,
    )
    value = MAC_RE.sub("<redacted-address>", value)
    value = LONG_HEX_RE.sub("<redacted-hex>", value)
    lines = []
    for line in value.splitlines():
        if SECRET_WORD_RE.search(line):
            lines.append("<redacted-sensitive-line>")
        else:
            lines.append(line)
    return "\n".join(lines)


def sanitize_tree(value: Any, repo: Path | None = None) -> Any:
    if isinstance(value, dict):
        return {sanitize_text(str(k), repo): sanitize_tree(v, repo) for k, v in value.items()}
    if isinstance(value, list):
        return [sanitize_tree(v, repo) for v in value]
    if isinstance(value, str):
        return sanitize_text(value, repo)
    return value


def relative_path(path: Path, base: Path) -> str:
    try:
        return os.path.relpath(path.resolve(), base.resolve())
    except (OSError, ValueError):
        return path.name


def resolve_from(path: str | Path, base: Path) -> Path:
    resolved = Path(path)
    if not resolved.is_absolute():
        resolved = base / resolved
    return resolved.resolve()


def json_load(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        raise DashboardError(f"cannot read JSON {path.name}: {exc}") from exc


def json_write(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = json.dumps(value, indent=2, sort_keys=False, ensure_ascii=True) + "\n"
    path.write_text(data, encoding="utf-8")


def text_write(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_string(argv: Iterable[str], repo: Path) -> str:
    def quote(part: str) -> str:
        if re.fullmatch(r"[A-Za-z0-9_./:=+,-]+", part):
            return part
        return "'" + part.replace("'", "'\"'\"'") + "'"

    return sanitize_text(" ".join(quote(str(part)) for part in argv), repo)


def run_command(
    argv: list[str],
    *,
    repo: Path,
    cwd: Path | None = None,
    timeout: int = 120,
    extra_env: dict[str, str] | None = None,
    capture_path: Path | None = None,
) -> dict[str, Any]:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    started = utc_now()
    try:
        proc = subprocess.run(
            argv,
            cwd=str(cwd or repo),
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        if capture_path is not None:
            captured = proc.stdout or ""
            if proc.stderr:
                captured += ("\n" if captured else "") + proc.stderr
            text_write(capture_path, sanitize_text(captured, repo))
        combined = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail = "\n".join(combined[-8:])
        return {
            "command": command_string(argv, repo),
            "started_at": started,
            "exit_code": proc.returncode,
            "tail": sanitize_text(tail, repo),
        }
    except (OSError, subprocess.SubprocessError) as exc:
        return {
            "command": command_string(argv, repo),
            "started_at": started,
            "exit_code": None,
            "tail": sanitize_text(str(exc), repo),
        }


def git_identity(repo: Path) -> dict[str, Any]:
    commit = run_command(["git", "rev-parse", "HEAD"], repo=repo, timeout=10)
    status = run_command(["git", "status", "--porcelain"], repo=repo, timeout=10)
    commit_value = None
    if commit["exit_code"] == 0:
        raw = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=repo, capture_output=True, text=True, check=False
        ).stdout.strip()
        if re.fullmatch(r"[0-9a-f]{40}", raw):
            commit_value = raw
    dirty = None
    if status["exit_code"] == 0:
        raw_status = subprocess.run(
            ["git", "status", "--porcelain"], cwd=repo, capture_output=True, text=True, check=False
        ).stdout
        dirty = bool(raw_status.strip())
    return {"commit": commit_value, "dirty": dirty}


def percentile(values: list[float], quantile: float) -> float | None:
    """R-7/NumPy-style linear percentile for a non-empty finite sequence."""
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    fraction = position - lower
    return float(ordered[lower] + (ordered[upper] - ordered[lower]) * fraction)


def distribution(values: Iterable[float]) -> dict[str, Any]:
    data = [float(value) for value in values if math.isfinite(float(value))]
    if not data:
        return {"count": 0, "p50": None, "p95": None, "p99": None, "max": None, "mad": None}
    median = statistics.median(data)
    mad = statistics.median(abs(value - median) for value in data)
    return {
        "count": len(data),
        "p50": round(float(percentile(data, 0.50)), 3),
        "p95": round(float(percentile(data, 0.95)), 3),
        "p99": round(float(percentile(data, 0.99)), 3),
        "max": round(max(data), 3),
        "mad": round(float(mad), 3),
    }


def parse_latency_line(line: str) -> dict[str, Any]:
    marker = line.find(LATENCY_MARKER)
    if marker < 0:
        raise DashboardError("not a latency line")
    canonical = line[marker:].strip()
    match = LATENCY_LINE_RE.fullmatch(canonical)
    if not match:
        raise DashboardError("malformed latency line")
    offsets: dict[str, int | None] = {}
    for raw_token in match.group("body").split():
        token = LATENCY_TOKEN_RE.fullmatch(raw_token)
        if not token:
            raise DashboardError("malformed latency token")
        phase = token.group("phase")
        if phase not in PHASES or phase in offsets:
            raise DashboardError("unknown or duplicate latency phase")
        offsets[phase] = int(token.group("value")) if token.group("value") is not None else None
    missing_tokens = [phase for phase in PHASES if phase not in offsets]
    if missing_tokens:
        raise DashboardError("latency line omitted phase tokens")
    non_null = [(phase, offsets[phase]) for phase in PHASES if offsets[phase] is not None]
    inversions = []
    previous_phase = None
    previous_value = None
    for phase, value in non_null:
        assert value is not None
        if previous_value is not None and value < previous_value:
            inversions.append({"before": previous_phase, "after": phase})
        previous_phase, previous_value = phase, value
    deltas: dict[str, int | None] = {PHASES[0]: offsets[PHASES[0]]}
    for previous, phase in zip(PHASES, PHASES[1:]):
        before, current = offsets[previous], offsets[phase]
        deltas[phase] = current - before if before is not None and current is not None else None
    return {
        "raw": canonical,
        "offsets_ms": offsets,
        "durations_ms": deltas,
        "inversions": inversions,
        "successful": offsets["bolt"] is not None and not inversions,
        "complete_trace": all(offsets[phase] is not None for phase in PHASES) and not inversions,
    }


def parse_latency_log(
    path: Path,
    *,
    warmup: int = 0,
    attempts: int | None = None,
    rejected: int = 0,
    timed_out: int = 0,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    warmup = checked_int(warmup, "warmup", maximum=1_000_000)
    rejected = checked_int(rejected, "rejected", maximum=1_000_000)
    timed_out = checked_int(timed_out, "timed_out", maximum=1_000_000)
    if attempts is not None:
        attempts = checked_int(attempts, "attempts", maximum=1_000_000)
    samples: list[dict[str, Any]] = []
    rejected_lines = 0
    malformed_lines = 0
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if LATENCY_MARKER not in line:
                rejected_lines += 1
                continue
            try:
                samples.append(parse_latency_line(line))
            except DashboardError:
                malformed_lines += 1

    measured = samples[warmup:]
    successful = [sample for sample in measured if sample["successful"]]
    no_grant = [sample for sample in measured if not sample["successful"] and not sample["inversions"]]
    invalid = [sample for sample in measured if sample["inversions"]]
    complete = [sample for sample in successful if sample["complete_trace"]]
    incomplete = [sample for sample in successful if not sample["complete_trace"]]
    minimum_attempts = (
        len(successful)
        + len(no_grant)
        + len(invalid)
        + malformed_lines
        + rejected
        + timed_out
    )
    if attempts is not None and attempts < minimum_attempts:
        raise DashboardError(
            f"attempts={attempts} is smaller than the {minimum_attempts} observed/classified outcomes"
        )
    unobserved = attempts - minimum_attempts if attempts is not None else None

    end_to_end = distribution(sample["offsets_ms"]["bolt"] for sample in successful)
    phase_rows = []
    for phase in PHASES:
        offsets = distribution(
            sample["offsets_ms"][phase]
            for sample in successful
            if sample["offsets_ms"][phase] is not None
        )
        durations = distribution(
            sample["durations_ms"][phase]
            for sample in successful
            if sample["durations_ms"][phase] is not None
        )
        phase_rows.append({"phase": phase, "offset_ms": offsets, "duration_ms": durations})

    summary = {
        "status": "measured" if samples else "failed",
        "source": path.name,
        "unit": "ms",
        "warmup_excluded": min(warmup, len(samples)),
        "allowlisted_lines": len(samples),
        "rejected_line_count": rejected_lines,
        "malformed_latency_lines": malformed_lines,
        "outcomes": {
            "attempts": attempts,
            "attempts_status": "measured" if attempts is not None else "partial",
            "successful": len(successful),
            "complete_traces": len(complete),
            "incomplete_traces": len(incomplete),
            "no_grant": len(no_grant),
            "invalid": len(invalid) + malformed_lines,
            "rejected": rejected,
            "timed_out": timed_out,
            "unobserved": unobserved,
        },
        "end_to_end_ms": end_to_end,
        "phases": phase_rows,
    }
    safe_samples = [
        {
            "raw": sample["raw"],
            "offsets_ms": sample["offsets_ms"],
            "durations_ms": sample["durations_ms"],
            "successful": sample["successful"],
            "complete_trace": sample["complete_trace"],
            "inversions": sample["inversions"],
        }
        for sample in samples
    ]
    return summary, safe_samples


def normalize_runtime_snapshot(data: Any) -> dict[str, Any]:
    if not isinstance(data, dict):
        raise DashboardError("runtime snapshot must be a JSON object")
    allowed = {"schema", "hardware", "stacks", "cpu", "uwb", "spi", "gates", "notes"}
    unknown = set(data) - allowed
    if unknown:
        raise DashboardError(f"runtime snapshot has unsupported keys: {', '.join(sorted(unknown))}")
    if data.get("schema") != 1:
        raise DashboardError("runtime snapshot schema must be 1")

    hardware_in = data.get("hardware") or {}
    if not isinstance(hardware_in, dict):
        raise DashboardError("hardware must be an object")
    for field in ("used", "authorized"):
        if not isinstance(hardware_in.get(field, False), bool):
            raise DashboardError(f"hardware.{field} must be a boolean")
    hardware = {
        "used": hardware_in.get("used", False),
        "authorized": hardware_in.get("authorized", False),
        "fixture": safe_label(hardware_in.get("fixture", "not-set"), "hardware.fixture"),
        "workload": safe_label(hardware_in.get("workload", "not-set"), "hardware.workload"),
        "sample_count": checked_int(hardware_in.get("sample_count", 0), "hardware.sample_count"),
    }

    stacks = []
    for index, item in enumerate(data.get("stacks") or []):
        if not isinstance(item, dict):
            raise DashboardError(f"stacks[{index}] must be an object")
        thread = safe_label(item.get("thread"), f"stacks[{index}].thread")
        configured = checked_int(item.get("configured_bytes"), f"stacks[{index}].configured_bytes", minimum=1)
        peak = checked_int(item.get("peak_bytes"), f"stacks[{index}].peak_bytes")
        if peak > configured:
            raise DashboardError(f"stacks[{index}] peak exceeds configured size")
        stacks.append({
            "thread": thread,
            "configured_bytes": configured,
            "peak_bytes": peak,
            "headroom_bytes": configured - peak,
            "used_pct": round(100.0 * peak / configured, 2),
            "method": safe_label(item.get("method", "unspecified"), f"stacks[{index}].method"),
            "samples": checked_int(item.get("samples", 1), f"stacks[{index}].samples", minimum=1),
        })

    cpu_in = data.get("cpu")
    cpu: dict[str, Any] | None = None
    if cpu_in is not None:
        if not isinstance(cpu_in, dict):
            raise DashboardError("cpu must be an object")
        threads = []
        for index, item in enumerate(cpu_in.get("threads") or []):
            if not isinstance(item, dict):
                raise DashboardError(f"cpu.threads[{index}] must be an object")
            threads.append({
                "thread": safe_label(item.get("thread"), f"cpu.threads[{index}].thread"),
                "utilization_pct": checked_float(
                    item.get("utilization_pct"), f"cpu.threads[{index}].utilization_pct"
                ),
            })
        cpu = {
            "busy_pct": checked_float(cpu_in.get("busy_pct"), "cpu.busy_pct"),
            "method": safe_label(cpu_in.get("method", "unspecified"), "cpu.method"),
            "threads": threads,
        }

    uwb_in = data.get("uwb")
    uwb: dict[str, Any] | None = None
    if uwb_in is not None:
        if not isinstance(uwb_in, dict):
            raise DashboardError("uwb must be an object")
        ticks_per_us = checked_float(uwb_in.get("ticks_per_us", 250), "uwb.ticks_per_us", minimum=0.001, maximum=1e9)
        bucket_width_us = checked_int(uwb_in.get("bucket_width_us", 250), "uwb.bucket_width_us", minimum=1)
        phases: dict[str, Any] = {}
        for phase, item in (uwb_in.get("phases") or {}).items():
            if phase not in {"prepoll", "response", "final"} or not isinstance(item, dict):
                raise DashboardError(f"unsupported UWB phase {phase!r}")
            histogram = item.get("histogram") or []
            if not isinstance(histogram, list) or not all(isinstance(value, int) and value >= 0 for value in histogram):
                raise DashboardError(f"uwb.{phase}.histogram must be non-negative integers")
            samples = checked_int(item.get("samples"), f"uwb.{phase}.samples")
            if sum(histogram) != samples:
                raise DashboardError(f"uwb.{phase} histogram sum does not equal samples")
            min_ticks = checked_int(item.get("min_ticks"), f"uwb.{phase}.min_ticks")
            max_ticks = checked_int(item.get("max_ticks"), f"uwb.{phase}.max_ticks")
            if max_ticks < min_ticks:
                raise DashboardError(f"uwb.{phase} max is smaller than min")
            phases[phase] = {
                "samples": samples,
                "min_ticks": min_ticks,
                "max_ticks": max_ticks,
                "min_us": round(min_ticks / ticks_per_us, 3),
                "max_us": round(max_ticks / ticks_per_us, 3),
                "histogram": [
                    {
                        "start_us": index * bucket_width_us,
                        "end_us": (index + 1) * bucket_width_us,
                        "count": count,
                        "saturating": index == len(histogram) - 1,
                    }
                    for index, count in enumerate(histogram)
                ],
                "deadline_misses": checked_int(
                    item.get("deadline_misses", 0), f"uwb.{phase}.deadline_misses"
                ),
                "arm_failures": checked_int(item.get("arm_failures", 0), f"uwb.{phase}.arm_failures"),
            }
        uwb = {
            "raw_unit": "dw3000_hi32_ticks",
            "ticks_per_us": ticks_per_us,
            "bucket_width_us": bucket_width_us,
            "deadline_us": checked_float(uwb_in.get("deadline_us", 1836), "uwb.deadline_us", minimum=0.001, maximum=1e9),
            "phases": phases,
        }

    spi_in = data.get("spi")
    spi: dict[str, Any] | None = None
    if spi_in is not None:
        if not isinstance(spi_in, dict):
            raise DashboardError("spi must be an object")
        phases: dict[str, Any] = {}
        for phase, item in (spi_in.get("phases") or {}).items():
            if phase not in {"poll_arm", "response_arm", "final_arm"} or not isinstance(item, dict):
                raise DashboardError(f"unsupported SPI phase {phase!r}")
            samples = checked_int(item.get("samples"), f"spi.{phase}.samples")
            row = {"samples": samples}
            for field in (
                "transactions", "reads", "writes", "wire_bytes", "errors", "timeouts",
                "max_transactions", "max_wire_bytes",
            ):
                row[field] = checked_int(item.get(field, 0), f"spi.{phase}.{field}")
            row["avg_transactions"] = round(row["transactions"] / samples, 3) if samples else None
            row["avg_wire_bytes"] = round(row["wire_bytes"] / samples, 3) if samples else None
            phases[phase] = row
        spi = {
            "frequency_hz": checked_int(spi_in.get("frequency_hz"), "spi.frequency_hz", minimum=1),
            "phases": phases,
        }

    gates = []
    for index, item in enumerate(data.get("gates") or []):
        if not isinstance(item, dict):
            raise DashboardError(f"gates[{index}] must be an object")
        gate_status = item.get("status")
        if gate_status not in {"pass", "fail", "not-run"}:
            raise DashboardError(f"gates[{index}].status must be pass, fail, or not-run")
        gates.append({
            "name": safe_label(item.get("name"), f"gates[{index}].name"),
            "status": gate_status,
            "detail": safe_label(item.get("detail", "not supplied"), f"gates[{index}].detail"),
        })

    notes = []
    for index, note in enumerate(data.get("notes") or []):
        notes.append(safe_label(note, f"notes[{index}]"))
    return {
        "status": "measured",
        "hardware": hardware,
        "stacks": stacks,
        "cpu": cpu,
        "uwb": uwb,
        "spi": spi,
        "gates": gates,
        "notes": notes,
    }


def load_python_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise DashboardError(f"cannot load {path.name}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def normalize_config(report: dict[str, Any], repo: Path) -> dict[str, Any]:
    source = report.get("config") or {}
    kconfig = source.get("kconfig") or {}
    return {
        "board": sanitize_text(str(source.get("board") or "unknown"), repo),
        "image": sanitize_text(str(source.get("image") or "unknown"), repo),
        "extra_conf_file": sanitize_text(str(source.get("extra_conf_file") or ""), repo),
        "zephyr_version": sanitize_text(str(source.get("zephyr_version") or "unknown"), repo),
        "ncs_version": sanitize_text(str(source.get("ncs_version") or "unknown"), repo),
        "toolchain": sanitize_text(str(source.get("toolchain") or "unknown"), repo),
        "kconfig": {
            key: sanitize_text(str(value), repo)
            for key, value in sorted(kconfig.items())
            if key in CONFIG_ALLOWLIST
        },
    }


def collect_memory(size_report: dict[str, Any], baseline_doc: dict[str, Any], repo: Path) -> dict[str, Any]:
    compare = load_python_module("zephyr_opt_size_compare", repo / "scripts/cdk-size-compare.py")
    config = normalize_config(size_report, repo)
    report_for_compare = dict(size_report)
    report_for_compare["config"] = dict(size_report.get("config") or {})
    key = compare.config_key(report_for_compare.get("config"))
    recorded = compare.baselines_of(baseline_doc)
    baseline = recorded.get(key)
    reasons: list[dict[str, Any]] = []
    if baseline is not None:
        reasons = [
            {"field": field, "baseline": sanitize_tree(before, repo), "current": sanitize_tree(after, repo)}
            for field, before, after in compare.config_diff(baseline, report_for_compare)
        ]
    comparable = baseline is not None and not reasons

    regions = []
    gate_failures: list[str] = []
    current_regions = size_report.get("regions") or {}
    for name, current in current_regions.items():
        if not isinstance(current, dict):
            continue
        row: dict[str, Any] = {
            "name": str(name),
            "size": current.get("size"),
            "used": current.get("used"),
            "free": current.get("free"),
            "pct": current.get("pct"),
            "baseline_used": None,
            "baseline_free": None,
            "delta": None,
            "free_floor": None,
            "delta_cap": None,
            "gate": "unknown",
        }
        if comparable:
            base_region = baseline.get("regions", {}).get(name)
            gate = baseline.get("gate", {})
            low = str(name).lower()
            if base_region:
                row["baseline_used"] = base_region.get("used")
                row["baseline_free"] = base_region.get("free")
                row["delta"] = current.get("used", 0) - base_region.get("used", 0)
                row["free_floor"] = gate.get(f"{low}_free_floor")
                row["delta_cap"] = gate.get(f"{low}_delta_cap")
                _, failures = compare.gate_region(
                    name,
                    baseline,
                    report_for_compare,
                    row["free_floor"],
                    row["delta_cap"],
                    False,
                )
                row["gate"] = "fail" if failures else "pass"
                gate_failures.extend(failures)
        regions.append(row)

    movers = []
    if comparable:
        for delta, name, before, after in compare.movers(baseline, report_for_compare, 15):
            movers.append({"name": name, "baseline": before, "current": after, "delta": delta})

    crosscheck = []
    for kind, values in (size_report.get("crosscheck") or {}).items():
        if not isinstance(values, dict):
            continue
        report_key = "ram_report" if kind == "ram" else "rom_report"
        crosscheck.append({
            "kind": kind,
            "regions": values.get("regions"),
            "zephyr_report": values.get(report_key),
            "delta": values.get("delta"),
        })

    sections = [
        {"name": sanitize_text(str(name), repo), "bytes": value}
        for name, value in sorted(
            (size_report.get("sections") or {}).items(), key=lambda item: -int(item[1])
        )[:15]
    ]
    baseline_reference = None
    if baseline_doc.get("primary") in recorded:
        primary = recorded[baseline_doc["primary"]]
        baseline_reference = {
            "key": baseline_doc["primary"],
            "config": normalize_config(primary, repo),
            "regions": [dict({"name": name}, **values) for name, values in primary.get("regions", {}).items()],
            "reference_only": not comparable,
        }
    return {
        "status": "measured",
        "config": config,
        "baseline_key": key,
        "baseline_found": baseline is not None,
        "comparable": comparable,
        "comparison_reasons": reasons,
        "regions": regions,
        "crosscheck": crosscheck,
        "top_sections": sections,
        "top_movers": movers,
        "gate_failures": [sanitize_text(reason, repo) for reason in gate_failures],
        "baseline_reference": baseline_reference,
        "lto_attribution_caveat": True,
    }


def unavailable_memory(baseline_doc: dict[str, Any], repo: Path) -> dict[str, Any]:
    recorded = baseline_doc.get("baselines") or {}
    primary_key = baseline_doc.get("primary")
    primary = recorded.get(primary_key) if isinstance(recorded, dict) else None
    reference = None
    if isinstance(primary, dict):
        reference = {
            "key": primary_key,
            "config": normalize_config(primary, repo),
            "regions": [dict({"name": name}, **values) for name, values in primary.get("regions", {}).items()],
            "reference_only": True,
        }
    return {
        "status": "unavailable",
        "config": None,
        "baseline_key": None,
        "baseline_found": bool(reference),
        "comparable": False,
        "comparison_reasons": [],
        "regions": [],
        "crosscheck": [],
        "top_sections": [],
        "top_movers": [],
        "gate_failures": [],
        "baseline_reference": reference,
        "lto_attribution_caveat": True,
    }


def artifact_entry(label: str, href: str, kind: str, path: Path | None = None) -> dict[str, Any]:
    out = {"label": label, "href": href.replace(os.sep, "/"), "kind": kind}
    if path and path.is_file():
        out["bytes"] = path.stat().st_size
        out["sha256"] = sha256_file(path)
    return out


def read_kconfig_axis(config_path: Path, repo: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not config_path.is_file():
        return values
    allowed = set(CONFIG_ALLOWLIST)
    with config_path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if line.startswith("# ") and line.endswith(" is not set"):
                key = line[2:-11]
                if key in allowed:
                    values[key] = "n"
                continue
            key, sep, value = line.partition("=")
            if sep and key in allowed:
                values[key] = sanitize_text(value.strip('"'), repo)
    return dict(sorted(values.items()))


def collect_compiler_stack_usage(image_build: Path, repo: Path) -> dict[str, Any]:
    """Parse GCC -fstack-usage records without persisting source paths."""
    files = sorted(image_build.rglob("*.su")) if image_build.is_dir() else []
    rows: dict[str, dict[str, Any]] = {}
    malformed = 0
    for path in files[:10_000]:
        try:
            if path.stat().st_size > 5 * 1024 * 1024:
                malformed += 1
                continue
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            malformed += 1
            continue
        for line in lines:
            fields = line.split("\t")
            if len(fields) < 3 or not fields[1].isdigit():
                malformed += 1
                continue
            function = sanitize_text(fields[0].rsplit(":", 1)[-1], repo)
            if not function or len(function) > 240:
                malformed += 1
                continue
            size = int(fields[1])
            qualifier = sanitize_text(fields[2], repo)[:80]
            current = rows.get(function)
            if current is None or size > current["bytes"]:
                rows[function] = {
                    "function": function,
                    "bytes": size,
                    "qualifier": qualifier,
                    "object": path.name,
                }
    ordered = sorted(rows.values(), key=lambda row: (-row["bytes"], row["function"]))[:100]
    return {
        "status": "measured" if ordered else "unavailable",
        "file_count": len(files),
        "malformed_records": malformed,
        "rows": ordered,
        "unit": "bytes",
        "method": "gcc-fstack-usage",
    }


def tool_version(name: str, argv: list[str], repo: Path) -> dict[str, Any]:
    path = shutil.which(name)
    if not path:
        return {"status": "unavailable", "version": None}
    result = run_command(argv, repo=repo, timeout=15)
    text = result.get("tail") or ""
    version = text.splitlines()[0][:160] if text else "installed"
    return {
        "status": "measured" if result["exit_code"] == 0 else "failed",
        "version": version,
    }


def choose_west_launcher() -> list[str]:
    if os.environ.get("ULTRAWIDELOCK_TOOLCHAIN") == "env":
        return ["west"]
    return ["nrfutil", "sdk-manager", "toolchain", "launch", "--ncs-version", NCS_VERSION, "--", "west"]


def collect_zephyr_tools(
    repo: Path,
    build: Path,
    run_tools: bool,
    commands: list[dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    image_build = build / SIZE_IMAGE
    out = {
        "target_inventory": status_record("not-run", "Zephyr targets were not requested"),
        "ram_report": status_record("unavailable", "ram.json is absent"),
        "rom_report": status_record("unavailable", "rom.json is absent"),
        "zephyr_dashboard": status_record("unavailable", "built-in dashboard is absent"),
    }
    if (image_build / "ram.json").is_file():
        out["ram_report"] = status_record("measured", "Zephyr ram_report JSON exists")
    if (image_build / "rom.json").is_file():
        out["rom_report"] = status_record("measured", "Zephyr rom_report JSON exists")
    if (image_build / "dashboard" / "index.html").is_file():
        out["zephyr_dashboard"] = status_record("measured", "built-in Zephyr dashboard exists")
    if not run_tools:
        return out
    workspace = repo / "workspace"
    if not workspace.exists() or not image_build.exists():
        detail = "workspace or nested image build is absent"
        out["target_inventory"] = status_record("unavailable", detail)
        return out
    launcher = choose_west_launcher()
    if not shutil.which(launcher[0]):
        out["target_inventory"] = status_record("unavailable", f"{launcher[0]} is not installed")
        return out
    inventory = run_command(
        launcher + ["build", "-d", str(image_build.resolve()), "-t", "help"],
        repo=repo,
        cwd=workspace,
        timeout=120,
        extra_env={"BROWSER": "true"},
    )
    commands.append(inventory)
    out["target_inventory"] = status_record(
        "measured" if inventory["exit_code"] == 0 else "failed",
        "nested image target inventory completed" if inventory["exit_code"] == 0 else inventory["tail"],
    )
    if inventory["exit_code"] != 0:
        return out
    for target, key, artifact in (
        ("ram_report", "ram_report", image_build / "ram.json"),
        ("rom_report", "rom_report", image_build / "rom.json"),
        ("dashboard", "zephyr_dashboard", image_build / "dashboard" / "index.html"),
    ):
        result = run_command(
            launcher + ["build", "-d", str(image_build.resolve()), "-t", target],
            repo=repo,
            cwd=workspace,
            timeout=1800,
            extra_env={"BROWSER": "true"},
        )
        commands.append(result)
        if result["exit_code"] == 0 and artifact.is_file():
            out[key] = status_record("measured", f"Zephyr {target} target produced its artifact")
        elif f"unknown target '{target}'" in (result["tail"] or ""):
            out[key] = status_record(
                "unavailable", f"the pinned Zephyr does not provide a {target} target"
            )
        else:
            detail = result["tail"] or f"{target} produced no expected artifact"
            out[key] = status_record("failed", detail)
    return out


def build_verdict(model: dict[str, Any]) -> dict[str, str]:
    runtime_gates = model.get("runtime", {}).get("gates") or []
    if model.get("memory", {}).get("gate_failures") or any(gate["status"] == "fail" for gate in runtime_gates):
        return {"code": "blocked", "label": "Blocked", "detail": "One or more explicit gates failed."}
    evidence = {item["id"]: item for item in model.get("evidence", [])}
    if any(item["status"] == "failed" for item in evidence.values()):
        return {"code": "failed", "label": "Collection failed", "detail": "At least one requested collector failed."}
    if evidence.get("workspace", {}).get("status") == "unavailable" or evidence.get("signing_key", {}).get("status") == "unavailable":
        return {
            "code": "setup-required",
            "label": "Setup required",
            "detail": "Firmware prerequisites are missing; no current image was measured.",
        }
    if model.get("memory", {}).get("status") != "measured":
        return {"code": "incomplete", "label": "Incomplete", "detail": "No current size report is available."}
    if evidence.get("hardware", {}).get("status") != "measured":
        return {
            "code": "software-only",
            "label": "Software evidence only",
            "detail": "Static evidence exists; hardware latency and reliability remain unmeasured.",
        }
    return {"code": "measured", "label": "Measured", "detail": "Static and supplied runtime evidence were collected."}


def recommended_action(model: dict[str, Any]) -> str:
    evidence = {item["id"]: item for item in model.get("evidence", [])}
    if evidence.get("workspace", {}).get("status") == "unavailable":
        return "Run make bootstrap when network and installation are authorized, then build a pristine debug lane."
    if evidence.get("signing_key", {}).get("status") == "unavailable":
        return "Run make dfu-key, then build a pristine DWM3001CDK lane."
    if model.get("memory", {}).get("status") != "measured":
        return "Run make build and make cdk-size for the selected CDK_BUILD, then recollect."
    if not model.get("memory", {}).get("comparable"):
        return "Build a profile that exactly matches a recorded baseline, or deliberately refresh the stale baseline outside this collector."
    if evidence.get("latency", {}).get("status") != "measured":
        return "Capture an authorized bench latency log and runtime snapshot from the exact flashed ELF."
    if evidence.get("hardware", {}).get("status") != "measured":
        return "Run the authorized control workload on hardware and supply its sanitized runtime snapshot."
    return "Choose one falsifiable optimization hypothesis and collect an interleaved control/candidate A/B."


def render_markdown(model: dict[str, Any]) -> str:
    verdict = model["verdict"]
    lines = [
        "# DWM3001CDK Zephyr optimization status\n",
        f"Run `{model['run']['id']}` · {model['run']['generated_at']} · **{verdict['label']}**\n",
        f"{verdict['detail']}\n",
    ]
    memory = model["memory"]
    lines.append("## Memory\n")
    if memory["status"] == "measured":
        lines.append("| Region | Used | Free | Delta | Gate |\n|---|---:|---:|---:|---|\n")
        for region in memory["regions"]:
            delta = "n/a" if region["delta"] is None else f"{region['delta']:+,} B"
            lines.append(
                f"| {region['name']} | {region['used']:,} B | **{region['free']:,} B** | {delta} | {region['gate']} |\n"
            )
        if not memory["comparable"]:
            lines.append("\nCurrent and baseline configurations are not comparable. No size delta is a gate result.\n")
    else:
        lines.append("No current ELF/size report was measured. Checked-in values are reference-only.\n")

    latency = model["latency"]
    lines.append("\n## Runtime\n")
    if latency["status"] == "measured":
        stats = latency["end_to_end_ms"]
        outcomes = latency["outcomes"]
        lines.append(
            f"Walk-up: {outcomes['successful']} successful, {outcomes['complete_traces']} complete traces; "
            f"p50 {stats['p50']} ms, p95 {stats['p95']} ms, p99 {stats['p99']} ms, max {stats['max']} ms.\n"
        )
    else:
        lines.append("Latency was not measured.\n")
    runtime = model["runtime"]
    uwb = runtime.get("uwb") if runtime["status"] == "measured" else None
    if uwb:
        misses = sum(item["deadline_misses"] for item in uwb["phases"].values())
        failures = sum(item["arm_failures"] for item in uwb["phases"].values())
        lines.append(f"UWB: {misses} deadline misses and {failures} arm failures in the supplied snapshot.\n")
    else:
        lines.append("UWB/SPI/stack hardware snapshot was not measured.\n")

    failed = [item for item in model["evidence"] if item["status"] == "failed"]
    missing = [item for item in model["evidence"] if item["status"] in {"unavailable", "not-run"}]
    lines.append("\n## Evidence\n")
    if failed:
        lines.append("Failed: " + ", ".join(item["label"] for item in failed) + ".\n")
    if missing:
        lines.append("Missing or not run: " + ", ".join(item["label"] for item in missing) + ".\n")
    lines.append(f"\nNext action: {model['next_action']}\n")
    lines.append("\nInspect `latest.json` for machine-readable detail and `index.html` for the dashboard.\n")
    return "".join(lines)


def render_html(model: dict[str, Any], tooling_dir: Path) -> str:
    template = (tooling_dir / "zephyr_opt_dashboard.html").read_text(encoding="utf-8")
    css = (tooling_dir / "zephyr_opt_dashboard.css").read_text(encoding="utf-8")
    javascript = (tooling_dir / "zephyr_opt_dashboard.js").read_text(encoding="utf-8")
    data = json.dumps(model, separators=(",", ":"), ensure_ascii=True)
    data = data.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    for placeholder, replacement in (
        ("/*__DASHBOARD_CSS__*/", css),
        ("/*__DASHBOARD_JS__*/", javascript),
        ("__DASHBOARD_DATA__", data),
    ):
        if template.count(placeholder) != 1:
            raise DashboardError(f"template placeholder {placeholder} is missing or duplicated")
        template = template.replace(placeholder, replacement)
    return template


def build_run_id(requested: str | None, generated_at: str, commit: str | None, profile: str) -> str:
    if requested:
        requested = requested.strip()
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", requested) or ".." in requested:
            raise DashboardError("run-id must be a safe filename using letters, digits, dot, underscore, or dash")
        return requested
    stamp = generated_at.replace("-", "").replace(":", "").replace("T", "-").replace("Z", "")
    short_commit = (commit or "unknown")[:12]
    clean_profile = re.sub(r"[^a-z0-9-]+", "-", profile.lower()).strip("-") or "unknown"
    return f"{stamp}-{short_commit}-{clean_profile}"


def collect(args: argparse.Namespace) -> dict[str, Any]:
    repo = Path(args.repo_root).resolve()
    if not (repo / "scripts/cdk-size.py").is_file():
        raise DashboardError("repo-root does not look like the UltraWideLock repository")
    output = resolve_from(args.output, repo)
    build = resolve_from(args.build, repo)
    baseline_path = resolve_from(args.baseline, repo)
    baseline_doc = json_load(baseline_path)
    if not isinstance(baseline_doc, dict):
        raise DashboardError("baseline must be a JSON object")
    generated_at = utc_now()
    identity = git_identity(repo)
    profile = safe_label(args.profile, "profile")
    experiment = safe_label(args.experiment, "experiment")
    run_id = build_run_id(args.run_id, generated_at, identity.get("commit"), profile)
    run_dir = output / "runs" / run_id
    artifacts_dir = run_dir / "artifacts"
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    commands: list[dict[str, Any]] = []
    artifacts: list[dict[str, Any]] = []
    evidence: list[dict[str, Any]] = []

    workspace_exists = (repo / "workspace").exists()
    evidence.append({
        "id": "workspace",
        "label": "NCS workspace",
        **status_record("measured" if workspace_exists else "unavailable", "workspace is present" if workspace_exists else "workspace is absent"),
    })
    signing_key_exists = any(
        path.is_file()
        for path in (
            repo / "apps/dwm3001cdk-lock/keys/mcuboot_ec_p256.pem",
            repo / "firmware/keys/mcuboot_ec_p256.pem",
        )
    )
    evidence.append({
        "id": "signing_key",
        "label": "Signing key",
        **status_record("measured" if signing_key_exists else "unavailable", "ignored signing key is present" if signing_key_exists else "ignored signing key is absent"),
    })

    if args.generate_size_report:
        elf = build / SIZE_IMAGE / "zephyr" / "zephyr.elf"
        generated_size = artifacts_dir / "size-report.json"
        if elf.is_file():
            command = [
                sys.executable,
                str(repo / "scripts/cdk-size.py"),
                "--build", str(build),
                "--image", SIZE_IMAGE,
                "--json", str(generated_size),
                "--quiet",
            ]
            result = run_command(command, repo=repo, timeout=300)
            commands.append(result)
            if result["exit_code"] != 0:
                evidence.append({"id": "size_generation", "label": "Size generation", **status_record("failed", result["tail"] or "cdk-size failed")})
        else:
            evidence.append({"id": "size_generation", "label": "Size generation", **status_record("unavailable", "nested application ELF is absent")})

    size_path = resolve_from(args.size_report, repo) if args.size_report else build / "size-report.json"
    generated_candidate = artifacts_dir / "size-report.json"
    if generated_candidate.is_file():
        size_path = generated_candidate
    if size_path.is_file():
        try:
            size_report = sanitize_tree(json_load(size_path), repo)
            if not isinstance(size_report, dict):
                raise DashboardError("size report must be a JSON object")
            memory = collect_memory(size_report, baseline_doc, repo)
            copied_size = artifacts_dir / "size-report.json"
            if size_path != copied_size:
                json_write(copied_size, size_report)
            href = relative_path(copied_size, output)
            artifacts.append(artifact_entry("Canonical size report", href, "json", copied_size))
            evidence.append({"id": "size_report", "label": "Current size report", **status_record("measured", "canonical cdk-size report loaded", href)})
        except DashboardError as exc:
            memory = unavailable_memory(baseline_doc, repo)
            evidence.append({"id": "size_report", "label": "Current size report", **status_record("failed", sanitize_text(str(exc), repo))})
    else:
        memory = unavailable_memory(baseline_doc, repo)
        evidence.append({"id": "size_report", "label": "Current size report", **status_record("unavailable", "size-report.json is absent")})

    zephyr_evidence = collect_zephyr_tools(repo, build, args.run_zephyr_tools, commands)
    image_build = build / SIZE_IMAGE
    for key, label in (
        ("target_inventory", "Zephyr target inventory"),
        ("ram_report", "Zephyr RAM report"),
        ("rom_report", "Zephyr ROM report"),
        ("zephyr_dashboard", "Built-in Zephyr dashboard"),
    ):
        record = zephyr_evidence[key]
        if key == "ram_report" and (image_build / "ram.json").is_file():
            sanitized = sanitize_tree(json_load(image_build / "ram.json"), repo)
            destination = artifacts_dir / "ram.json"
            json_write(destination, sanitized)
            href = relative_path(destination, output)
            record["artifact"] = href
            artifacts.append(artifact_entry(label, href, "json", destination))
        elif key == "rom_report" and (image_build / "rom.json").is_file():
            sanitized = sanitize_tree(json_load(image_build / "rom.json"), repo)
            destination = artifacts_dir / "rom.json"
            json_write(destination, sanitized)
            href = relative_path(destination, output)
            record["artifact"] = href
            artifacts.append(artifact_entry(label, href, "json", destination))
        elif key == "zephyr_dashboard" and (image_build / "dashboard" / "index.html").is_file():
            href = relative_path(image_build / "dashboard" / "index.html", output)
            record["artifact"] = href
            artifacts.append(artifact_entry(label, href, "html"))
        evidence.append({"id": key, "label": label, **record})

    config_axis = read_kconfig_axis(image_build / "zephyr" / ".config", repo)
    if config_axis:
        axis_path = artifacts_dir / "config-axis.json"
        json_write(axis_path, config_axis)
        href = relative_path(axis_path, output)
        artifacts.append(artifact_entry("Allowlisted Kconfig axis", href, "json", axis_path))

    compiler_stack = collect_compiler_stack_usage(image_build, repo)
    memory["compiler_stack"] = compiler_stack
    if compiler_stack["status"] == "measured":
        stack_path = artifacts_dir / "compiler-stack-usage.json"
        json_write(stack_path, compiler_stack)
        href = relative_path(stack_path, output)
        artifacts.append(artifact_entry("Compiler stack estimates", href, "json", stack_path))
        evidence.append({
            "id": "compiler_stack",
            "label": "Compiler stack estimates",
            **status_record("measured", f"parsed {compiler_stack['file_count']} GCC .su files", href),
        })
    else:
        evidence.append({
            "id": "compiler_stack",
            "label": "Compiler stack estimates",
            **status_record("unavailable", "no GCC .su records; use the static-stack diagnostic overlay"),
        })

    pahole_record = status_record("not-run", "pahole layout collection was not requested")
    if args.run_pahole:
        elf = image_build / "zephyr" / "zephyr.elf"
        if not elf.is_file():
            pahole_record = status_record("unavailable", "nested application ELF is absent")
        elif not shutil.which("pahole"):
            pahole_record = status_record("unavailable", "pahole is not installed")
        else:
            pahole_path = artifacts_dir / "pahole-sizes.txt"
            result = run_command(
                ["pahole", "--sizes", str(elf)],
                repo=repo,
                timeout=300,
                capture_path=pahole_path,
            )
            commands.append(result)
            if result["exit_code"] == 0 and pahole_path.is_file() and pahole_path.stat().st_size:
                href = relative_path(pahole_path, output)
                artifacts.append(artifact_entry("pahole structure sizes", href, "text", pahole_path))
                pahole_record = status_record("measured", "structure-size report collected", href)
            else:
                pahole_record = status_record("failed", result["tail"] or "pahole produced no output")
    evidence.append({"id": "pahole_layout", "label": "pahole data layout", **pahole_record})

    map_path = image_build / "zephyr" / "zephyr.map"
    if map_path.is_file():
        artifacts.append(artifact_entry("Linker map (local, unsanitized)", relative_path(map_path, output), "map"))
    partitions = build / "partitions.yml"
    if partitions.is_file() and partitions.stat().st_size <= 2 * 1024 * 1024:
        destination = artifacts_dir / "partitions.yml"
        text_write(destination, sanitize_text(partitions.read_text(encoding="utf-8", errors="replace"), repo) + "\n")
        artifacts.append(artifact_entry("Partition map", relative_path(destination, output), "yaml", destination))

    latency = {
        "status": "unavailable",
        "unit": "ms",
        "outcomes": {"attempts": None, "attempts_status": "not-measured", "successful": 0},
        "end_to_end_ms": distribution([]),
        "phases": [],
    }
    if args.latency_log:
        latency_path = resolve_from(args.latency_log, repo)
        if latency_path.is_file():
            try:
                latency, samples = parse_latency_log(
                    latency_path,
                    warmup=args.warmup,
                    attempts=args.attempts,
                    rejected=args.rejected,
                    timed_out=args.timed_out,
                )
                samples_path = artifacts_dir / "latency-samples.json"
                json_write(samples_path, {"schema": 1, "samples": samples})
                href = relative_path(samples_path, output)
                artifacts.append(artifact_entry("Sanitized latency samples", href, "json", samples_path))
                evidence.append({"id": "latency", "label": "Walk-up latency", **status_record(latency["status"], "allowlisted latency lines parsed", href)})
            except (OSError, DashboardError) as exc:
                latency["status"] = "failed"
                evidence.append({"id": "latency", "label": "Walk-up latency", **status_record("failed", sanitize_text(str(exc), repo))})
        else:
            evidence.append({"id": "latency", "label": "Walk-up latency", **status_record("unavailable", "latency log is absent")})
    else:
        evidence.append({"id": "latency", "label": "Walk-up latency", **status_record("not-run", "no latency log supplied")})

    runtime: dict[str, Any] = {
        "status": "unavailable",
        "hardware": {"used": False, "authorized": False, "fixture": "not-set", "workload": "not-set", "sample_count": 0},
        "stacks": [], "cpu": None, "uwb": None, "spi": None, "gates": [], "notes": [],
    }
    if args.snapshot:
        snapshot_path = resolve_from(args.snapshot, repo)
        if snapshot_path.is_file():
            try:
                runtime = normalize_runtime_snapshot(json_load(snapshot_path))
                normalized_path = artifacts_dir / "runtime-snapshot.json"
                json_write(normalized_path, runtime)
                href = relative_path(normalized_path, output)
                artifacts.append(artifact_entry("Sanitized runtime snapshot", href, "json", normalized_path))
                evidence.append({"id": "runtime_snapshot", "label": "Runtime snapshot", **status_record("measured", "validated runtime snapshot loaded", href)})
            except DashboardError as exc:
                runtime["status"] = "failed"
                evidence.append({"id": "runtime_snapshot", "label": "Runtime snapshot", **status_record("failed", sanitize_text(str(exc), repo))})
        else:
            evidence.append({"id": "runtime_snapshot", "label": "Runtime snapshot", **status_record("unavailable", "runtime snapshot is absent")})
    else:
        evidence.append({"id": "runtime_snapshot", "label": "Runtime snapshot", **status_record("not-run", "no runtime snapshot supplied")})
    hardware_used = runtime.get("status") == "measured" and runtime.get("hardware", {}).get("used")
    hardware_authorized = runtime.get("hardware", {}).get("authorized") if hardware_used else False
    hardware_measured = hardware_used and hardware_authorized
    if hardware_used and not hardware_authorized:
        hardware_status = status_record("failed", "runtime snapshot says hardware use was not authorized")
    elif hardware_measured:
        hardware_status = status_record("measured", "anonymized authorized hardware workload supplied")
    else:
        hardware_status = status_record("not-run", "no hardware workload supplied")
    evidence.append({
        "id": "hardware",
        "label": "Hardware workload",
        **hardware_status,
    })

    tools = {
        "python": {"status": "measured", "version": sys.version.split()[0]},
        "pahole": tool_version("pahole", ["pahole", "--version"], repo),
        "puncover": tool_version("puncover", ["puncover", "--version"], repo),
    }
    evidence.append({
        "id": "pahole",
        "label": "pahole",
        **status_record(tools["pahole"]["status"], tools["pahole"]["version"] or "not installed"),
    })
    evidence.append({
        "id": "puncover",
        "label": "puncover",
        **status_record(tools["puncover"]["status"], tools["puncover"]["version"] or "not installed; optional third-party tool"),
    })

    run = {
        "id": run_id,
        "generated_at": generated_at,
        "commit": identity.get("commit"),
        "dirty": identity.get("dirty"),
        "profile": profile,
        "experiment": experiment,
        "control_run": safe_label(args.control_run, "control-run", allow_empty=True) or None,
        "build_dir": relative_path(build, repo) if str(build).startswith(str(repo)) else build.name,
        "board": "decawave_dwm3001cdk",
        "image": SIZE_IMAGE,
    }
    model: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "run": run,
        "tools": tools,
        "memory": memory,
        "latency": latency,
        "runtime": runtime,
        "evidence": evidence,
        "artifacts": artifacts,
        "warnings": [],
        "next_action": "",
        "verdict": {},
    }
    if memory["status"] == "measured" and not memory["comparable"]:
        model["warnings"].append("Current size data is not comparable with the selected baseline.")
    if latency.get("outcomes", {}).get("attempts_status") == "partial" and latency["status"] == "measured":
        model["warnings"].append("Latency failure denominator is partial because total attempts were not supplied.")
    if memory.get("lto_attribution_caveat"):
        model["warnings"].append("Top symbol movers under LTO are indicative and do not drive the gate.")
    model["next_action"] = recommended_action(model)
    model["verdict"] = build_verdict(model)

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "run": run,
        "tools": tools,
        "evidence": evidence,
        "artifacts": artifacts,
        "invocation": sanitize_text(command_string(sys.argv, repo), repo),
    }
    metrics = {
        "schema_version": SCHEMA_VERSION,
        "run_id": run_id,
        "memory": memory,
        "latency": latency,
        "runtime": runtime,
        "warnings": model["warnings"],
        "verdict": model["verdict"],
    }
    json_write(run_dir / "manifest.json", manifest)
    json_write(run_dir / "metrics.json", metrics)
    command_lines = [entry["command"] + f"\n  exit={entry['exit_code']}\n  {entry['tail']}" for entry in commands]
    text_write(run_dir / "commands.txt", "\n\n".join(command_lines) + ("\n" if command_lines else "No external collectors executed.\n"))
    json_write(output / "latest.json", model)
    text_write(output / "latest.md", render_markdown(model))
    text_write(output / "index.html", render_html(model, Path(__file__).resolve().parent))
    return model


def render_existing(args: argparse.Namespace) -> dict[str, Any]:
    source = Path(args.input).resolve()
    model = sanitize_tree(json_load(source), repo_default())
    if not isinstance(model, dict):
        raise DashboardError("dashboard model must be a JSON object")
    if model.get("schema_version") != SCHEMA_VERSION:
        raise DashboardError(f"unsupported dashboard schema {model.get('schema_version')!r}")
    output = Path(args.output).resolve() if args.output else source.parent
    output.mkdir(parents=True, exist_ok=True)
    json_write(output / "latest.json", model)
    text_write(output / "latest.md", render_markdown(model))
    text_write(output / "index.html", render_html(model, Path(__file__).resolve().parent))
    return model


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    collect_parser = sub.add_parser("collect", help="collect evidence and render the dashboard")
    collect_parser.add_argument("--repo-root", default=str(repo_default()))
    collect_parser.add_argument("--build", default="build/cdk-matter")
    collect_parser.add_argument("--output", default="internal/zephyr-opt/dashboard")
    collect_parser.add_argument("--baseline", default="apps/dwm3001cdk-lock/size-baseline.json")
    collect_parser.add_argument("--size-report")
    collect_parser.add_argument("--latency-log")
    collect_parser.add_argument("--snapshot")
    collect_parser.add_argument("--profile", default="debug")
    collect_parser.add_argument("--experiment", default="control")
    collect_parser.add_argument("--control-run", default="")
    collect_parser.add_argument("--run-id")
    collect_parser.add_argument("--warmup", type=int, default=0)
    collect_parser.add_argument("--attempts", type=int)
    collect_parser.add_argument("--rejected", type=int, default=0)
    collect_parser.add_argument("--timed-out", type=int, default=0)
    collect_parser.add_argument("--generate-size-report", action="store_true")
    collect_parser.add_argument("--run-zephyr-tools", action="store_true")
    collect_parser.add_argument("--run-pahole", action="store_true")

    render_parser = sub.add_parser("render", help="rerender an existing latest.json")
    render_parser.add_argument("--input", required=True)
    render_parser.add_argument("--output")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        if args.command == "collect":
            model = collect(args)
        else:
            model = render_existing(args)
    except DashboardError as exc:
        sys.stderr.write(f"zephyr-opt-dashboard: {exc}\n")
        return 2
    verdict = model.get("verdict", {})
    sys.stdout.write(
        f"dashboard: {verdict.get('label', 'rendered')}\n"
        f"  html  {Path(args.output if getattr(args, 'output', None) else Path(args.input).parent) / 'index.html'}\n"
    )
    return 1 if verdict.get("code") in {"blocked", "failed"} else 0


if __name__ == "__main__":
    raise SystemExit(main())
