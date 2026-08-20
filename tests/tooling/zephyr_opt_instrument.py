#!/usr/bin/env python3
"""Interactive real-board workflow for the DWM3001CDK optimization dashboard."""

from __future__ import annotations

import argparse
import datetime as dt
import functools
import http.server
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
from typing import Sequence
import webbrowser


LATENCY_MARKER = "ultrawidelock-lat:"
RUN_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,120}$")


class WorkflowError(RuntimeError):
    """A concise error that explains how to unblock the workflow."""


class Ui:
    def __init__(self, stream=sys.stdout) -> None:
        self.stream = stream
        self.color = bool(getattr(stream, "isatty", lambda: False)()) and not os.environ.get("NO_COLOR")

    def _paint(self, code: str, value: str) -> str:
        return f"\033[{code}m{value}\033[0m" if self.color else value

    def banner(self) -> None:
        print(file=self.stream)
        print(f"  {self._paint('1;36', 'DWM3001CDK INSTRUMENT')}", file=self.stream)
        print(f"  {self._paint('2', 'build  flash  observe  render')}", file=self.stream)
        print(file=self.stream)

    def step(self, number: int, total: int, title: str, detail: str = "") -> None:
        marker = self._paint("1;33", f"{number}/{total}")
        print(f"  {marker}  {self._paint('1', title)}", file=self.stream)
        if detail:
            print(f"       {self._paint('2', detail)}", file=self.stream)
        self.stream.flush()

    def ok(self, detail: str) -> None:
        print(f"       {self._paint('32', 'OK')}  {detail}", file=self.stream)
        self.stream.flush()

    def note(self, detail: str) -> None:
        print(f"       {self._paint('2', detail)}", file=self.stream)
        self.stream.flush()


def utc_run_id() -> str:
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"instrument-{stamp}"


def checked_run_id(value: str | None) -> str:
    run_id = value or utc_run_id()
    if not RUN_ID_RE.fullmatch(run_id):
        raise WorkflowError("run id must contain only letters, numbers, dot, underscore, or hyphen")
    return run_id


def checked_count(value: int | None, field: str, *, optional: bool = False) -> int | None:
    if value is None and optional:
        return None
    if value is None or value < 0 or value > 1_000_000:
        raise WorkflowError(f"{field} must be an integer from 0 through 1000000")
    return value


def check_loopback_port(port: int) -> None:
    if port == 0:
        return
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as candidate:
        candidate.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            candidate.bind(("127.0.0.1", port))
        except OSError as exc:
            raise WorkflowError(
                f"127.0.0.1:{port} is unavailable; set INSTRUMENT_PORT to another port"
            ) from exc


def workspace_status(workspace: Path) -> str:
    markers = (".ultrawidelock-fetch-done", ".ultrawidelock-patches.sha256")
    if all((workspace / marker).is_file() for marker in markers):
        return "ready"
    if not workspace.exists() and not workspace.is_symlink():
        return "absent"
    return "incomplete"


def ensure_signing_key(path: Path) -> bool:
    """Create the checkout-local P-256 key without touching an existing file."""
    if path.is_file():
        return False
    if path.exists():
        raise WorkflowError("the signing-key path exists but is not a regular file")
    path.parent.mkdir(parents=True, exist_ok=True)

    openssl = shutil.which("openssl")
    if openssl:
        descriptor, temporary_name = tempfile.mkstemp(prefix=".mcuboot-key-", dir=path.parent)
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            completed = subprocess.run(
                [openssl, "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(temporary)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            if completed.returncode != 0:
                raise WorkflowError("openssl could not generate the MCUboot signing key")
            os.chmod(temporary, 0o600)
            if path.exists():
                return False
            os.replace(temporary, path)
            return True
        finally:
            temporary.unlink(missing_ok=True)

    try:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError as exc:
        raise WorkflowError("creating the signing key requires openssl or Python cryptography") from exc
    payload = ec.generate_private_key(ec.SECP256R1()).private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        return False
    with os.fdopen(descriptor, "wb") as key_file:
        key_file.write(payload)
    return True


def make_command(make: str, target: str, **variables: str | int) -> list[str]:
    command = [make, "--no-print-directory", target]
    command.extend(f"{name}={value}" for name, value in variables.items() if value != "")
    return command


def collect_command(
    python: str,
    collector: Path,
    repo: Path,
    build: Path,
    output: Path,
    capture: Path,
    profile: str,
    experiment: str,
    run_id: str,
    attempts: int | None,
    warmup: int,
    rejected: int,
    timed_out: int,
) -> list[str]:
    command = [
        python,
        str(collector),
        "collect",
        "--repo-root",
        str(repo),
        "--build",
        str(build),
        "--output",
        str(output),
        "--latency-log",
        str(capture),
        "--profile",
        profile,
        "--experiment",
        experiment,
        "--run-id",
        run_id,
        "--warmup",
        str(warmup),
        "--rejected",
        str(rejected),
        "--timed-out",
        str(timed_out),
        "--run-zephyr-tools",
        "--run-pahole",
    ]
    if attempts is not None:
        command.extend(("--attempts", str(attempts)))
    return command


def run_checked(command: Sequence[str], repo: Path) -> None:
    try:
        completed = subprocess.run(command, cwd=repo, check=False)
    except OSError as exc:
        raise WorkflowError(f"could not start {command[0]}: {exc}") from exc
    if completed.returncode != 0:
        raise WorkflowError(f"command failed with exit {completed.returncode}: {command[0]} {command[2]}")


def stop_process_group(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    for sent_signal, timeout in ((signal.SIGINT, 4.0), (signal.SIGTERM, 2.0)):
        try:
            os.killpg(process.pid, sent_signal)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=timeout)
            return
        except subprocess.TimeoutExpired:
            continue
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def wait_for_enter_or_exit(process: subprocess.Popen[bytes]) -> str:
    while process.poll() is None:
        ready, _, _ = select.select([sys.stdin], [], [], 0.2)
        if ready:
            sys.stdin.readline()
            return "operator"
    return "process"


def capture_rtt(command: Sequence[str], repo: Path, capture: Path, ui: Ui) -> int:
    capture.parent.mkdir(parents=True, exist_ok=True)
    try:
        process = subprocess.Popen(
            command,
            cwd=repo,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    except OSError as exc:
        raise WorkflowError(f"could not start RTT monitor: {exc}") from exc

    latency_records = 0
    read_error: list[BaseException] = []

    def copy_output() -> None:
        nonlocal latency_records
        try:
            assert process.stdout is not None
            with capture.open("wb") as raw:
                while True:
                    line = process.stdout.readline()
                    if not line:
                        break
                    raw.write(line)
                    raw.flush()
                    sys.stdout.buffer.write(line)
                    sys.stdout.buffer.flush()
                    if LATENCY_MARKER.encode() in line:
                        latency_records += 1
        except BaseException as exc:  # surfaced on the foreground thread below
            read_error.append(exc)

    reader = threading.Thread(target=copy_output, name="dwm3001cdk-rtt", daemon=True)
    reader.start()
    ui.note("Walk up with the phone now. Press Enter when the run is complete.")
    stopped_by_operator = False
    try:
        stopped_by_operator = wait_for_enter_or_exit(process) == "operator"
    except KeyboardInterrupt:
        stopped_by_operator = True
        print(file=ui.stream)
    finally:
        if stopped_by_operator:
            stop_process_group(process)
        reader.join(timeout=5.0)
        if reader.is_alive():
            stop_process_group(process)
            reader.join(timeout=2.0)

    if read_error:
        raise WorkflowError(f"could not save RTT capture: {read_error[0]}")
    if not stopped_by_operator:
        raise WorkflowError(f"RTT monitor exited before capture was finished with {process.returncode}")
    ui.ok(f"{latency_records} latency record(s) captured in {capture.name}")
    return latency_records


def prompt_attempts(current: int | None, latency_records: int) -> int | None:
    if current is not None:
        return current
    prompt = (
        f"\n  Total physical attempts [{latency_records} observed records, blank = unknown]: "
    )
    while True:
        try:
            value = input(prompt).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return None
        if not value:
            return None
        try:
            return checked_count(int(value), "attempts", optional=True)
        except (ValueError, WorkflowError):
            print("  Enter a whole number, or leave it blank.", file=sys.stderr)


class LocalDashboardHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, format: str, *args: object) -> None:
        return


def serve_dashboard(output: Path, port: int, ui: Ui) -> None:
    handler = functools.partial(LocalDashboardHandler, directory=str(output))
    try:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    except OSError as exc:
        raise WorkflowError(f"cannot serve the dashboard on 127.0.0.1:{port}: {exc}") from exc
    actual_port = server.server_address[1]
    url = f"http://127.0.0.1:{actual_port}/"
    thread = threading.Thread(target=server.serve_forever, name="zephyr-opt-dashboard", daemon=True)
    thread.start()
    try:
        opened = webbrowser.open(url, new=2)
        ui.ok(f"serving {url}")
        if not opened:
            ui.note("The browser did not acknowledge the request. Open the URL above manually.")
        ui.note("Press Enter to stop the local server and exit cleanly.")
        try:
            input()
        except (EOFError, KeyboardInterrupt):
            print(file=ui.stream)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)
    ui.ok("server stopped; dashboard files remain under internal/zephyr-opt/dashboard")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--make", default="make")
    parser.add_argument("--build", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--capture-dir", required=True)
    parser.add_argument("--sign-key", required=True)
    parser.add_argument("--conf", required=True)
    parser.add_argument("--profile", default="thread+lto")
    parser.add_argument("--experiment", default="real-board")
    parser.add_argument("--run-id")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--attempts", type=int)
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--rejected", type=int, default=0)
    parser.add_argument("--timed-out", type=int, default=0)
    return parser


def run(args: argparse.Namespace) -> None:
    if not sys.stdin.isatty():
        raise WorkflowError("make instrument requires an interactive terminal and will not flash from CI")

    repo = Path(args.repo_root).resolve()
    build = Path(args.build).resolve()
    output = Path(args.output).resolve()
    capture_dir = Path(args.capture_dir).resolve()
    sign_key = Path(args.sign_key).resolve()
    collector = repo / "tests/tooling/zephyr_opt_dashboard.py"
    workspace = repo / "workspace"
    workspace_state = workspace_status(workspace)
    run_id = checked_run_id(args.run_id)
    attempts = checked_count(args.attempts, "attempts", optional=True)
    warmup = checked_count(args.warmup, "warmup")
    rejected = checked_count(args.rejected, "rejected")
    timed_out = checked_count(args.timed_out, "timed_out")
    if args.port < 0 or args.port > 65535:
        raise WorkflowError("port must be from 0 through 65535")
    if not collector.is_file():
        raise WorkflowError("dashboard collector is missing from tests/tooling")
    if shutil.which(args.make) is None:
        raise WorkflowError(f"make executable not found: {args.make}")
    if shutil.which("probe-rs") is None:
        raise WorkflowError("probe-rs is required for RTT; install it from https://probe.rs/")
    check_loopback_port(args.port)
    if workspace_state == "incomplete":
        raise WorkflowError(
            "workspace exists without completed bootstrap markers; inspect it and run make bootstrap explicitly"
        )

    ui = Ui()
    ui.banner()
    ui.note("Bench image only. Flash preserves settings; this workflow never runs flash-erase.")
    print(file=ui.stream)

    ui.step(1, 7, "Prepare the signing key and Zephyr workspace")
    if ensure_signing_key(sign_key):
        ui.ok("generated the ignored P-256 signing key; back it up securely")
    else:
        ui.ok("signing key already present")
    if workspace_state == "absent":
        run_checked(make_command(args.make, "bootstrap"), repo)
        if workspace_status(workspace) != "ready":
            raise WorkflowError("bootstrap returned without a complete workspace")
    else:
        ui.ok("workspace already present")

    ui.step(2, 7, "Build the pristine bench image", build.name)
    run_checked(
        make_command(
            args.make,
            "build",
            PRISTINE=1,
            CDK_BUILD=str(build),
            CDK_CONF=args.conf,
        ),
        repo,
    )

    ui.step(3, 7, "Measure linker flash and RAM")
    run_checked(make_command(args.make, "cdk-size", CDK_BUILD=str(build)), repo)

    ui.step(4, 7, "Flash the instrumented image", "plain flash; commissioned state is preserved")
    run_checked(make_command(args.make, "flash", CDK_BUILD=str(build)), repo)

    ui.step(5, 7, "Capture real-board RTT", "the exact flashed ELF is enforced by make monitor")
    capture = capture_dir / f"{run_id}.log"
    latency_records = capture_rtt(
        make_command(args.make, "monitor", CDK_RTT_BUILD=str(build)),
        repo,
        capture,
        ui,
    )
    attempts = prompt_attempts(attempts, latency_records)

    ui.step(6, 7, "Collect and render optimization evidence")
    run_checked(
        collect_command(
            sys.executable,
            collector,
            repo,
            build,
            output,
            capture,
            args.profile,
            args.experiment,
            run_id,
            attempts,
            warmup,
            rejected,
            timed_out,
        ),
        repo,
    )

    ui.step(7, 7, "Open the dashboard")
    serve_dashboard(output, args.port, ui)
    print(file=ui.stream)


def main() -> int:
    try:
        run(build_parser().parse_args())
    except WorkflowError as exc:
        print(f"\n  instrument: {exc}\n", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\n  instrument: interrupted\n", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
