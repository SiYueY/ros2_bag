#!/usr/bin/env python3
"""Phase 0 gitlink verification and release dependency metadata (stdlib only)."""
import argparse
import configparser
import json
from pathlib import Path
import subprocess

DEPENDENCIES = {
    "concurrentqueue": "https://github.com/cameron314/concurrentqueue.git",
    "sockpp": "https://github.com/fpagliughi/sockpp.git",
    "json": "https://github.com/nlohmann/json.git",
    "yaml-cpp": "https://github.com/jbeder/yaml-cpp.git",
}


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def verify(root):
    """Return verified SHAs; reject unexpected paths, dirty trees and wrong URLs."""
    config = configparser.ConfigParser()
    config.read(root / ".gitmodules")
    expected_paths = {f"third_party/{name}" for name in DEPENDENCIES}
    actual_paths = {config[section].get("path") for section in config.sections()}
    if actual_paths != expected_paths or len(config.sections()) != len(DEPENDENCIES):
        raise ValueError(".gitmodules must declare exactly four expected independent paths")
    shas = {}
    for name, url in DEPENDENCIES.items():
        relative = f"third_party/{name}"
        sections = [s for s in config.sections() if config[s].get("path") == relative]
        if config[sections[0]].get("url") != url:
            raise ValueError(f"unexpected upstream URL: {relative}")
        record = git(root, "ls-files", "--stage", "--", relative).split()
        if len(record) != 4 or record[0] != "160000" or record[2] != "0":
            raise ValueError(f"missing/conflicted gitlink: {relative}")
        dependency = root / relative
        if not (dependency / ".git").exists():
            raise ValueError(f"uninitialized submodule: {relative}")
        sha = git(dependency, "rev-parse", "HEAD")
        if sha != record[1]:
            raise ValueError(f"checkout does not match gitlink: {relative}")
        if git(dependency, "status", "--porcelain", "--untracked-files=all"):
            raise ValueError(f"dirty dependency: {relative}")
        recursive = git(dependency, "submodule", "status", "--recursive")
        if any(line.startswith(("-", "+", "U")) for line in recursive.splitlines()):
            raise ValueError(f"invalid recursive dependency: {relative}")
        shas[name] = sha
    return shas


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        shas = verify(args.root.resolve())
        if args.output:
            data = {"schema_version": 1, "source_commit": git(args.root, "rev-parse", "HEAD"),
                    "dependencies": shas}
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(data, indent=2) + "\n")
        print(json.dumps({"status": "PASS", "dependencies": shas}, sort_keys=True))
    except (ValueError, subprocess.CalledProcessError, OSError) as error:
        parser.exit(1, f"dependency verification FAILED: {error}\n")


if __name__ == "__main__":
    main()
