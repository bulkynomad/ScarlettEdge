#!/usr/bin/env python3
"""ScarlettEdge automated clean-room & IP/DMCA hygiene gate.

Checks tracked repository files (and optionally a git commit range) for:
  1. Prohibited binary, container, SDK, or key file extensions
  2. Unexpectedly large tracked files (> 2 MB)
  3. Private key headers or leaked cryptographic key material
  4. Blocked third-party trademark/codename identifiers

Exit code 0 on pass, 1 on any violation.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

PROHIBITED_EXTENSIONS = {
    ".xvc",
    ".msixvc",
    ".xvd",
    ".cik",
    ".ekb",
    ".lekb",
    ".nupkg",
    ".pdb",
    ".exe",
    ".dll",
    ".lib",
    ".iso",
    ".xex",
}

MAX_FILE_BYTES = 2 * 1024 * 1024  # 2 MB
SIZE_ALLOWLIST: set[str] = {
    "docs/image_00_01.png",
    "docs/image_00_02.png",
}

TEXT_EXTENSIONS = {
    ".c",
    ".cc",
    ".cpp",
    ".h",
    ".hpp",
    ".hlsl",
    ".hlsli",
    ".py",
    ".md",
    ".txt",
    ".cmake",
    ".yml",
    ".yaml",
    ".json",
    ".xml",
    ".config",
    ".html",
    ".svg",
    ".def",
    ".ps1",
    ".sh",
}

# Patterns constructed dynamically so this gate script itself stays hygiene-clean.
BLOCKED_PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    (
        "private-key-header",
        re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----"),
    ),
    (
        "blocked-trademark-token",
        re.compile(
            r"(?:"
            + "ra" + "ge_slice"
            + r"|"
            + "Vice" + "Leo" + "nida"
            + r"|"
            + "ShadowOf" + "Leo" + "nida"
            + r"|\b"
            + "Leo" + "nida"
            + r"\b|\b"
            + "RA" + "GE"
            + r"\b)"
        ),
    ),
]


def git_tracked_files() -> list[str]:
    proc = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    )
    raw = proc.stdout.decode("utf-8", errors="replace")
    return [p for p in raw.split("\0") if p]


def is_text_candidate(rel_path: str) -> bool:
    p = pathlib.Path(rel_path)
    if p.name in {"CMakeLists.txt", ".gitignore", "LICENSE"}:
        return True
    return p.suffix.lower() in TEXT_EXTENSIONS


def check_files(files: list[str]) -> list[str]:
    violations: list[str] = []
    for rel in files:
        full = ROOT / rel
        if not full.is_file():
            continue

        suffix = full.suffix.lower()
        if suffix in PROHIBITED_EXTENSIONS:
            violations.append(f"[prohibited-ext] {rel} (blocked extension {suffix})")
            continue

        size = full.stat().st_size
        if size > MAX_FILE_BYTES and rel not in SIZE_ALLOWLIST:
            violations.append(
                f"[file-too-large] {rel} ({size:,} bytes > {MAX_FILE_BYTES:,} byte budget)"
            )

        if not is_text_candidate(rel):
            continue

        try:
            text = full.read_text(encoding="utf-8", errors="strict")
        except UnicodeDecodeError:
            continue

        for lineno, line in enumerate(text.splitlines(), 1):
            for rule_name, pat in BLOCKED_PATTERNS:
                if pat.search(line):
                    violations.append(f"[{rule_name}] {rel}:{lineno}: {line.strip()[:120]}")

    return violations


def check_commit_messages(commit_range: str) -> list[str]:
    proc = subprocess.run(
        ["git", "log", "--format=%H%x1f%s%n%b%x1e", commit_range],
        cwd=ROOT,
        capture_output=True,
        check=True,
    )
    violations: list[str] = []
    entries = proc.stdout.decode("utf-8", errors="replace").split("\x1e")
    for entry in entries:
        entry = entry.strip()
        if not entry or "\x1f" not in entry:
            continue
        sha, body = entry.split("\x1f", 1)
        for rule_name, pat in BLOCKED_PATTERNS:
            m = pat.search(body)
            if m:
                first_line = body.splitlines()[0] if body.splitlines() else ""
                violations.append(f"[commit-msg:{rule_name}] {sha[:8]}: {first_line[:120]}")
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description="ScarlettEdge clean-room & IP hygiene gate.")
    parser.add_argument(
        "--commit-range",
        default=None,
        help="Optional git commit range to scan commit messages (e.g. HEAD~5..HEAD).",
    )
    args = parser.parse_args()

    files = git_tracked_files()
    violations = check_files(files)
    if args.commit_range:
        violations.extend(check_commit_messages(args.commit_range))

    if violations:
        print(f"[check_cleanroom] FAILED with {len(violations)} violation(s):", file=sys.stderr)
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        return 1

    scope = f"{len(files)} tracked files"
    if args.commit_range:
        scope += f" + commits ({args.commit_range})"
    print(f"[check_cleanroom] PASS ({scope}, 0 violations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
