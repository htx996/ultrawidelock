#!/usr/bin/env python3
"""Deterministic tests for the DWM3001CDK optimization dashboard."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = ROOT / "tests/tooling/zephyr_opt_dashboard.py"
FIXTURES = ROOT / "tests/tooling/fixtures/zephyr-opt"
SPEC = importlib.util.spec_from_file_location("zephyr_opt_dashboard", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
DASHBOARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DASHBOARD)


class LatencyTests(unittest.TestCase):
    def test_latency_log_separates_outcomes_and_computes_percentiles(self) -> None:
        summary, samples = DASHBOARD.parse_latency_log(
            FIXTURES / "latency.log",
            warmup=1,
            attempts=8,
            rejected=1,
            timed_out=1,
        )
        self.assertEqual(summary["status"], "measured")
        self.assertEqual(summary["outcomes"]["successful"], 3)
        self.assertEqual(summary["outcomes"]["no_grant"], 1)
        self.assertEqual(summary["outcomes"]["invalid"], 2)
        self.assertEqual(summary["outcomes"]["unobserved"], 0)
        self.assertEqual(summary["end_to_end_ms"]["p50"], 767.0)
        self.assertEqual(summary["end_to_end_ms"]["p95"], 808.4)
        self.assertEqual(len(samples), 6)

    def test_malformed_latency_counts_against_attempt_denominator(self) -> None:
        with self.assertRaisesRegex(DASHBOARD.DashboardError, "observed/classified"):
            DASHBOARD.parse_latency_log(FIXTURES / "latency.log", warmup=1, attempts=5)

    def test_latency_line_requires_every_phase_token(self) -> None:
        with self.assertRaisesRegex(DASHBOARD.DashboardError, "omitted phase"):
            DASHBOARD.parse_latency_line("ultrawidelock-lat: connect+1 bolt+2 ms")


class RuntimeTests(unittest.TestCase):
    def test_snapshot_normalizes_units_and_stack_headroom(self) -> None:
        raw = json.loads((FIXTURES / "runtime-snapshot.json").read_text(encoding="utf-8"))
        snapshot = DASHBOARD.normalize_runtime_snapshot(raw)
        self.assertEqual(snapshot["status"], "measured")
        self.assertEqual(snapshot["stacks"][0]["headroom_bytes"], 1360)
        self.assertEqual(snapshot["uwb"]["phases"]["prepoll"]["max_us"], 926.0)
        self.assertEqual(snapshot["spi"]["phases"]["poll_arm"]["avg_transactions"], 5.0)

    def test_snapshot_rejects_string_booleans(self) -> None:
        raw = json.loads((FIXTURES / "runtime-snapshot.json").read_text(encoding="utf-8"))
        raw["hardware"]["authorized"] = "false"
        with self.assertRaisesRegex(DASHBOARD.DashboardError, "must be a boolean"):
            DASHBOARD.normalize_runtime_snapshot(raw)

    def test_snapshot_rejects_sensitive_labels(self) -> None:
        raw = json.loads((FIXTURES / "runtime-snapshot.json").read_text(encoding="utf-8"))
        raw["hardware"]["fixture"] = "credential-id"
        with self.assertRaisesRegex(DASHBOARD.DashboardError, "looks sensitive"):
            DASHBOARD.normalize_runtime_snapshot(raw)


class SafetyTests(unittest.TestCase):
    def test_run_id_cannot_escape_the_run_directory(self) -> None:
        with self.assertRaisesRegex(DASHBOARD.DashboardError, "safe filename"):
            DASHBOARD.build_run_id("../outside", "2026-01-01T00:00:00Z", None, "debug")

    def test_failed_evidence_is_not_hidden_by_missing_setup(self) -> None:
        model = {
            "memory": {"gate_failures": []},
            "runtime": {"gates": []},
            "evidence": [
                {"id": "workspace", "status": "unavailable"},
                {"id": "hardware", "status": "failed"},
            ],
        }
        self.assertEqual(DASHBOARD.build_verdict(model)["code"], "failed")

    def test_absent_zephyr_target_is_unavailable_not_failed(self) -> None:
        # NCS v3.3.0 provides ram_report/rom_report but no `dashboard` ninja
        # target; a target the pinned Zephyr never had must not block the run.
        def fake_run(argv, **kwargs):
            target = argv[-1]
            if target == "dashboard":
                return {
                    "command": " ".join(argv),
                    "started_at": "",
                    "exit_code": 1,
                    "tail": "ninja: error: unknown target 'dashboard'",
                }
            if target in ("ram_report", "rom_report"):
                (image_build / f"{target.split('_')[0]}.json").write_text("{}", encoding="utf-8")
            return {"command": " ".join(argv), "started_at": "", "exit_code": 0, "tail": ""}

        with tempfile.TemporaryDirectory(prefix="uwl-zephyr-tools-test-") as temporary:
            repo = Path(temporary)
            (repo / "workspace").mkdir()
            image_build = repo / "build" / DASHBOARD.SIZE_IMAGE
            image_build.mkdir(parents=True)
            with mock.patch.object(DASHBOARD, "run_command", fake_run), \
                    mock.patch.object(DASHBOARD.shutil, "which", lambda _: "/usr/bin/west"):
                out = DASHBOARD.collect_zephyr_tools(repo, repo / "build", True, [])
        self.assertEqual(out["zephyr_dashboard"]["status"], "unavailable")
        self.assertEqual(out["ram_report"]["status"], "measured")
        self.assertEqual(out["rom_report"]["status"], "measured")

    def test_sanitizer_cleans_dictionary_keys_as_well_as_values(self) -> None:
        cleaned = DASHBOARD.sanitize_tree({"/Users/<user>/field": "/home/<user>/value"})
        serialized = json.dumps(cleaned)
        self.assertNotIn("/Users/", serialized)
        self.assertNotIn("/home/", serialized)


class MemoryTests(unittest.TestCase):
    def test_matching_fixture_is_comparable_and_gated(self) -> None:
        current = json.loads((FIXTURES / "size-report.json").read_text(encoding="utf-8"))
        baseline = json.loads((FIXTURES / "baseline.json").read_text(encoding="utf-8"))
        memory = DASHBOARD.collect_memory(current, baseline, ROOT)
        self.assertTrue(memory["comparable"])
        self.assertEqual(memory["gate_failures"], [])
        self.assertEqual({row["gate"] for row in memory["regions"]}, {"pass"})
        flash = next(row for row in memory["regions"] if row["name"] == "FLASH")
        self.assertEqual(flash["delta"], -576)

    def test_config_mismatch_suppresses_deltas_and_gates(self) -> None:
        current = json.loads((FIXTURES / "size-report.json").read_text(encoding="utf-8"))
        baseline = json.loads((FIXTURES / "baseline.json").read_text(encoding="utf-8"))
        current["config"]["toolchain"] = "different-toolchain"
        memory = DASHBOARD.collect_memory(current, baseline, ROOT)
        self.assertFalse(memory["comparable"])
        self.assertTrue(memory["comparison_reasons"])
        self.assertTrue(all(row["delta"] is None and row["gate"] == "unknown" for row in memory["regions"]))

    def test_compiler_stack_parser_drops_source_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uwl-stack-usage-") as temporary:
            image = Path(temporary)
            usage = image / "module.su"
            usage.write_text(
                "/Users/<user>/source.c:10:2:hot_path\t144\tstatic\n"
                "/home/<user>/source.c:20:2:cold_path\t64\tdynamic,bounded\n",
                encoding="utf-8",
            )
            report = DASHBOARD.collect_compiler_stack_usage(image, ROOT)
            self.assertEqual(report["status"], "measured")
            self.assertEqual(report["rows"][0]["function"], "hot_path")
            self.assertNotIn("/Users/", json.dumps(report))
            self.assertNotIn("/home/", json.dumps(report))


class EndToEndTests(unittest.TestCase):
    def collect_fixture(self, output: Path):
        args = argparse.Namespace(
            repo_root=str(ROOT),
            build="build/nonexistent-dashboard-test",
            output=str(output),
            baseline=str(FIXTURES / "baseline.json"),
            size_report=str(FIXTURES / "size-report.json"),
            latency_log=str(FIXTURES / "latency.log"),
            snapshot=str(FIXTURES / "runtime-snapshot.json"),
            profile="fixture",
            experiment="renderer-contract",
            control_run="",
            run_id="fixture-run",
            warmup=1,
            attempts=8,
            rejected=1,
            timed_out=1,
            generate_size_report=False,
            run_zephyr_tools=False,
            run_pahole=False,
        )
        return DASHBOARD.collect(args)

    def test_collect_renders_self_contained_dashboard_and_run_tree(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uwl-dashboard-test-") as temporary:
            output = Path(temporary) / "dashboard"
            model = self.collect_fixture(output)
            html = (output / "index.html").read_text(encoding="utf-8")
            latest = json.loads((output / "latest.json").read_text(encoding="utf-8"))
            manifest = json.loads((output / "runs/fixture-run/manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(model["memory"]["status"], "measured")
            self.assertEqual(model["latency"]["status"], "measured")
            self.assertEqual(model["runtime"]["status"], "measured")
            self.assertEqual(latest["schema_version"], 1)
            self.assertIn("runtime-snapshot.json", {Path(item["href"]).name for item in manifest["artifacts"]})
            self.assertNotIn("__DASHBOARD", html)
            self.assertNotIn("<script src=", html)
            self.assertNotIn("<link rel=", html)
            self.assertIn("window.__uwlDashboardReady = true", html)
            self.assertIn('dataset.dashboardReady = "true"', html)
            self.assertTrue((output / "latest.md").is_file())
            self.assertTrue((output / "runs/fixture-run/metrics.json").is_file())

    def test_collected_tree_has_no_private_paths_or_secret_shaped_values(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uwl-dashboard-privacy-") as temporary:
            output = Path(temporary) / "dashboard"
            self.collect_fixture(output)
            persisted = "\n".join(
                path.read_text(encoding="utf-8", errors="replace")
                for path in output.rglob("*")
                if path.is_file()
            )
            self.assertNotRegex(persisted, r"/(?:Users|home)/[^/\s]+")
            self.assertNotIn("mcuboot_ec_p256.pem", persisted)
            self.assertNotIn("credential-id", persisted)

    def test_schemas_are_valid_json_and_match_emitted_top_level_keys(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uwl-dashboard-schema-") as temporary:
            model = self.collect_fixture(Path(temporary) / "dashboard")
            schema = json.loads((ROOT / "tests/tooling/zephyr_opt_dashboard.schema.json").read_text())
            runtime_schema = json.loads((ROOT / "tests/tooling/zephyr_opt_runtime_snapshot.schema.json").read_text())
            self.assertEqual(schema["properties"]["schema_version"]["const"], model["schema_version"])
            self.assertTrue(set(schema["required"]).issubset(model))
            self.assertEqual(runtime_schema["properties"]["schema"]["const"], 1)


if __name__ == "__main__":
    unittest.main()
