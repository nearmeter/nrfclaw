#!/usr/bin/env python3
"""Build and verify the nRFClaw Home Assistant release package."""

from __future__ import annotations

import argparse
import hashlib
import json
import stat
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "integrations" / "home-assistant" / "custom_components" / "nrfclaw"
DEFAULT_OUTPUT = ROOT / "dist" / "nrfclaw-home-assistant.zip"
PREFIX = Path("custom_components") / "nrfclaw"
IGNORED_DIRS = {"__pycache__", ".pytest_cache", ".mypy_cache"}
IGNORED_SUFFIXES = {".pyc", ".pyo"}
ZIP_TIME = (1980, 1, 1, 0, 0, 0)


def source_files() -> dict[Path, Path]:
    if not SOURCE.is_dir():
        raise SystemExit(f"missing canonical integration directory: {SOURCE}")
    files: dict[Path, Path] = {}
    for path in sorted(SOURCE.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(SOURCE)
        if any(part in IGNORED_DIRS for part in rel.parts):
            continue
        if path.suffix in IGNORED_SUFFIXES:
            continue
        files[rel] = path
    if not files:
        raise SystemExit("canonical Home Assistant integration is empty")
    return files


def validate_manifest(files: dict[Path, Path]) -> dict:
    manifest_path = Path("manifest.json")
    if manifest_path not in files:
        raise SystemExit("manifest.json is missing")
    manifest = json.loads(files[manifest_path].read_text(encoding="utf-8"))
    if manifest.get("domain") != "nrfclaw":
        raise SystemExit("manifest domain must be 'nrfclaw'")
    if not str(manifest.get("version", "")).strip():
        raise SystemExit("manifest version is missing")
    if not manifest.get("config_flow"):
        raise SystemExit("manifest must keep config_flow enabled")
    return manifest


def build_zip(output: Path, files: dict[Path, Path]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for rel, src in files.items():
            info = zipfile.ZipInfo((PREFIX / rel).as_posix(), date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (stat.S_IMODE(src.stat().st_mode) & 0xFFFF) << 16
            archive.writestr(info, src.read_bytes(), compresslevel=9)


def verify_zip(output: Path, files: dict[Path, Path]) -> None:
    expected = {(PREFIX / rel).as_posix(): src for rel, src in files.items()}
    with zipfile.ZipFile(output, "r") as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise SystemExit("ZIP contains duplicate paths")
        actual = set(names)
        wanted = set(expected)
        if actual != wanted:
            for name in sorted(wanted - actual):
                print(f"MISSING from ZIP: {name}")
            for name in sorted(actual - wanted):
                print(f"EXTRA in ZIP:     {name}")
            raise SystemExit("Home Assistant package layout mismatch")
        for name, src in expected.items():
            if archive.read(name) != src.read_bytes():
                raise SystemExit(f"ZIP payload differs from canonical source: {name}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--sha256", type=Path, default=None)
    args = parser.parse_args()

    output = args.output if args.output.is_absolute() else (ROOT / args.output).resolve()
    files = source_files()
    manifest = validate_manifest(files)
    build_zip(output, files)
    verify_zip(output, files)

    digest = sha256_file(output)
    sha_path = args.sha256 or Path(str(output) + ".sha256")
    if not sha_path.is_absolute():
        sha_path = (ROOT / sha_path).resolve()
    sha_path.parent.mkdir(parents=True, exist_ok=True)
    sha_path.write_text(f"{digest}  {output.name}\n", encoding="utf-8")

    print(f"Home Assistant integration: nRFClaw {manifest['version']}")
    print(f"Files: {len(files)}")
    print(f"ZIP: {output}")
    print(f"SHA256: {digest}")
    print("HOME ASSISTANT PACKAGE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
