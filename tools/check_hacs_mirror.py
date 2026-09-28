#!/usr/bin/env python3
"""Check or refresh the HACS distribution mirror.

Canonical source:
  integrations/home-assistant/custom_components/nrfclaw

HACS-visible mirror:
  custom_components/nrfclaw
"""
from __future__ import annotations

import argparse
import filecmp
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "integrations" / "home-assistant" / "custom_components" / "nrfclaw"
MIRROR = ROOT / "custom_components" / "nrfclaw"

IGNORED_DIRS = {"__pycache__", ".pytest_cache", ".mypy_cache"}
IGNORED_SUFFIXES = {".pyc", ".pyo"}


def files_under(base: Path) -> dict[Path, Path]:
    out: dict[Path, Path] = {}
    for path in base.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(base)
        if any(part in IGNORED_DIRS for part in rel.parts):
            continue
        if path.suffix in IGNORED_SUFFIXES:
            continue
        out[rel] = path
    return out


def check() -> int:
    src = files_under(SOURCE)
    dst = files_under(MIRROR)
    missing = sorted(set(src) - set(dst))
    extra = sorted(set(dst) - set(src))
    changed = sorted(
        rel for rel in set(src) & set(dst)
        if not filecmp.cmp(src[rel], dst[rel], shallow=False)
    )

    for rel in missing:
        print(f"MISSING in HACS mirror: {rel}")
    for rel in extra:
        print(f"EXTRA in HACS mirror:   {rel}")
    for rel in changed:
        print(f"DIFFERS:                {rel}")

    if missing or extra or changed:
        print("HACS MIRROR: FAIL")
        return 1

    print(f"HACS MIRROR: PASS ({len(src)} files)")
    return 0


def sync() -> int:
    if MIRROR.exists():
        shutil.rmtree(MIRROR)
    shutil.copytree(SOURCE, MIRROR)
    print(f"Synced {SOURCE.relative_to(ROOT)} -> {MIRROR.relative_to(ROOT)}")
    return check()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--sync",
        action="store_true",
        help="replace the HACS mirror with the canonical integration tree",
    )
    args = parser.parse_args()
    return sync() if args.sync else check()


if __name__ == "__main__":
    raise SystemExit(main())
