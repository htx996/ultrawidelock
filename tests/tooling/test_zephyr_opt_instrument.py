#!/usr/bin/env python3
"""Deterministic tests for the interactive DWM3001CDK instrument workflow."""

from __future__ import annotations

import argparse
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = ROOT / "tests/tooling/zephyr_opt_instrument.py"
SPEC = importlib.util.spec_from_file_location("zephyr_opt_instrument", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
INSTRUMENT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTRUMENT)


class CommandTests(unittest.TestCase):
    def test_make_command_keeps_each_assignment_atomic(self) -> None:
        command = INSTRUMENT.make_command(
            "make",
            "build",
            PRISTINE=1,
            CDK_BUILD="/tmp/build path",
            CDK_CONF="one.conf;two.conf",
            EMPTY="",
        )
        self.assertEqual(command[:3], ["make", "--no-print-directory", "build"])
        self.assertIn("CDK_BUILD=/tmp/build path", command)
        self.assertIn("CDK_CONF=one.conf;two.conf", command)
        self.assertNotIn("EMPTY=", command)

    def test_collect_command_omits_unknown_attempt_denominator(self) -> None:
        command = INSTRUMENT.collect_command(
            "python3",
            ROOT / "tests/tooling/zephyr_opt_dashboard.py",
            ROOT,
            ROOT / "build/instrument",
            ROOT / "internal/dashboard",
            ROOT / "internal/capture.log",
            "thread+lto",
            "real-board",
            "fixture-run",
            None,
            0,
            0,
            0,
        )
        self.assertNotIn("--attempts", command)
        self.assertIn("--run-zephyr-tools", command)
        self.assertIn("--run-pahole", command)

    def test_collect_command_includes_explicit_attempt_denominator(self) -> None:
        command = INSTRUMENT.collect_command(
            "python3",
            ROOT / "tests/tooling/zephyr_opt_dashboard.py",
            ROOT,
            ROOT / "build/instrument",
            ROOT / "internal/dashboard",
            ROOT / "internal/capture.log",
            "thread+lto",
            "real-board",
            "fixture-run",
            1000,
            20,
            1,
            2,
        )
        index = command.index("--attempts")
        self.assertEqual(command[index + 1], "1000")


class SafetyTests(unittest.TestCase):
    def test_run_id_cannot_escape_capture_directory(self) -> None:
        with self.assertRaisesRegex(INSTRUMENT.WorkflowError, "run id"):
            INSTRUMENT.checked_run_id("../capture")

    def test_existing_signing_key_is_never_replaced(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uwl-instrument-key-") as temporary:
            key = Path(temporary) / "signing.pem"
            key.write_text("existing-key", encoding="utf-8")
            with mock.patch.object(INSTRUMENT.subprocess, "run") as run:
                generated = INSTRUMENT.ensure_signing_key(key)
            self.assertFalse(generated)
            self.assertEqual(key.read_text(encoding="utf-8"), "existing-key")
            run.assert_not_called()

    def test_incomplete_workspace_is_distinct_from_absent_and_ready(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uwl-instrument-workspace-") as temporary:
            workspace = Path(temporary) / "workspace"
            self.assertEqual(INSTRUMENT.workspace_status(workspace), "absent")
            workspace.mkdir()
            (workspace / ".ultrawidelock-fetch-done").write_text("ready\n", encoding="utf-8")
            self.assertEqual(INSTRUMENT.workspace_status(workspace), "incomplete")
            (workspace / ".ultrawidelock-patches.sha256").write_text("ready\n", encoding="utf-8")
            self.assertEqual(INSTRUMENT.workspace_status(workspace), "ready")

    def test_noninteractive_run_refuses_before_any_command(self) -> None:
        args = argparse.Namespace()
        with (
            mock.patch.object(INSTRUMENT.sys, "stdin", io.StringIO()),
            mock.patch.object(INSTRUMENT.subprocess, "run") as run,
            self.assertRaisesRegex(INSTRUMENT.WorkflowError, "interactive terminal"),
        ):
            INSTRUMENT.run(args)
        run.assert_not_called()

    def test_local_server_opens_browser_and_shuts_down(self) -> None:
        with tempfile.TemporaryDirectory(prefix="uwl-instrument-server-") as temporary:
            output = Path(temporary)
            (output / "index.html").write_text("<!doctype html><title>fixture</title>", encoding="utf-8")
            stream = io.StringIO()
            with (
                mock.patch.object(INSTRUMENT.webbrowser, "open", return_value=True) as opened,
                mock.patch("builtins.input", return_value=""),
            ):
                INSTRUMENT.serve_dashboard(output, 0, INSTRUMENT.Ui(stream))
            url = opened.call_args.args[0]
            self.assertRegex(url, r"^http://127\.0\.0\.1:\d+/$")
            self.assertIn("server stopped", stream.getvalue())

    def test_ready_workflow_orders_build_flash_capture_collect_and_serve(self) -> None:
        class InteractiveInput(io.StringIO):
            def isatty(self) -> bool:
                return True

        with tempfile.TemporaryDirectory(prefix="uwl-instrument-flow-") as temporary:
            repo = Path(temporary)
            collector = repo / "tests/tooling/zephyr_opt_dashboard.py"
            collector.parent.mkdir(parents=True)
            collector.write_text("# fixture\n", encoding="utf-8")
            marker = repo / "workspace/.ultrawidelock-fetch-done"
            marker.parent.mkdir()
            marker.write_text("ready\n", encoding="utf-8")
            (marker.parent / ".ultrawidelock-patches.sha256").write_text("ready\n", encoding="utf-8")
            args = argparse.Namespace(
                repo_root=str(repo),
                make="make",
                build=str(repo / "build/instrument"),
                output=str(repo / "internal/dashboard"),
                capture_dir=str(repo / "internal/captures"),
                sign_key=str(repo / "keys/signing.pem"),
                conf="one.conf;two.conf",
                profile="thread+lto",
                experiment="real-board",
                run_id="fixture-run",
                port=8765,
                attempts=10,
                warmup=2,
                rejected=1,
                timed_out=0,
            )
            quiet_ui = INSTRUMENT.Ui(io.StringIO())
            with (
                mock.patch.object(INSTRUMENT.sys, "stdin", InteractiveInput()),
                mock.patch.object(INSTRUMENT.shutil, "which", return_value="/tool"),
                mock.patch.object(INSTRUMENT, "check_loopback_port"),
                mock.patch.object(INSTRUMENT, "ensure_signing_key", return_value=False),
                mock.patch.object(INSTRUMENT, "Ui", return_value=quiet_ui),
                mock.patch.object(INSTRUMENT, "run_checked") as checked,
                mock.patch.object(INSTRUMENT, "capture_rtt", return_value=3),
                mock.patch.object(INSTRUMENT, "serve_dashboard") as served,
            ):
                INSTRUMENT.run(args)
            targets = [call.args[0][2] for call in checked.call_args_list]
            self.assertEqual(targets, ["build", "cdk-size", "flash", "collect"])
            served.assert_called_once()


if __name__ == "__main__":
    unittest.main()
