#!/usr/bin/env python3
"""Verify that independent Recorder processes cannot share a mutable output root."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def start(binary, config, env):
    return subprocess.Popen(
        [binary, "--config", str(config)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
    try:
        stdout, stderr = process.communicate(timeout=12)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise
    if process.returncode != 0:
        raise AssertionError(f"Recorder shutdown failed ({process.returncode}): {stderr}")
    return stdout, stderr


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_recorder_instance.py BINARY CONFIG_YAML")
    binary, source_config = sys.argv[1:]
    env = os.environ.copy()
    with tempfile.TemporaryDirectory(prefix="ros2-bag-instance-") as directory:
        root = Path(directory)
        output = root / "recordings"
        output.mkdir()
        alias = root / "alias"
        alias.symlink_to(output, target_is_directory=True)
        raw = Path(source_config).read_text()
        if "output_root: ./recordings" not in raw or "config_file: mcap.yaml" not in raw:
            raise AssertionError("unexpected sample Recorder configuration")
        raw = raw.replace("output_root: ./recordings", "output_root: " + str(output))
        raw = raw.replace("config_file: mcap.yaml", "config_file: " +
                          str(Path(source_config).resolve().parent / "mcap.yaml"))
        config = root / "recorder.yaml"
        config.write_text(raw)
        competing_config = root / "competing.yaml"
        competing_config.write_text(raw.replace("output_root: " + str(output),
                                                "output_root: " + str(alias)))

        first = start(binary, config, env)
        try:
            deadline = time.monotonic() + 10
            lock = output / ".ros2_bag_recorder.lock"
            while not lock.exists():
                if first.poll() is not None:
                    raise AssertionError("first Recorder exited unexpectedly")
                if time.monotonic() >= deadline:
                    raise AssertionError("first Recorder never acquired output lock")
                time.sleep(0.05)
            time.sleep(0.5)
            if first.poll() is not None:
                raise AssertionError("first Recorder exited after creating output lock")
            second = subprocess.run([binary, "--config", str(competing_config)],
                                    capture_output=True, text=True, env=env, timeout=10)
            if second.returncode == 0 or "already in use" not in second.stderr:
                raise AssertionError("concurrent Recorder was not rejected: " + second.stderr)
        finally:
            stop(first)

        # The lock file persists, but the OS lock was released by the first process.
        third = start(binary, config, env)
        try:
            time.sleep(0.75)
            if third.poll() is not None:
                raise AssertionError("Recorder cannot restart after lock release")
        finally:
            stop(third)
        if not lock.is_file():
            raise AssertionError("persistent lock file was unexpectedly removed")


if __name__ == "__main__":
    main()
