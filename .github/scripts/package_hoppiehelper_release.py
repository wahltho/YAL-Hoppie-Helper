#!/usr/bin/env python3
"""Build and verify GitHub release assets for YAL HoppieHelper."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path


PACKAGE_ROOT = "YAL_HoppieHelper"
PACKAGE_ID = "wahltho.yal-hoppiehelper"
REPOSITORY = "https://github.com/wahltho/YAL-Hoppie-Helper"
INSTALL_SCOPE = "xPlaneInstallation"
TARGET_PATH = "Resources/plugins/YAL_HoppieHelper"
ARCHIVE_PREFIX = "YAL-HoppieHelper"
RELEASE_TAG_PREFIX = "r"
SUPPORTED_PRODUCTS = (
    "zibo-737ng",
    "levelup-737ng",
)
PROTECTED_PATHS: tuple[str, ...] = ()

PACKAGE_MAP = (
    ("deploy/YAL_HoppieHelper/64/mac.xpl", "64/mac.xpl"),
    ("deploy/YAL_HoppieHelper/64/lin.xpl", "64/lin.xpl"),
    ("deploy/YAL_HoppieHelper/64/win.xpl", "64/win.xpl"),
    ("ACARS_CPDLC_GUIDE.md", "ACARS_CPDLC_GUIDE.md"),
    ("INSTALL.md", "INSTALL.md"),
    ("LICENSE", "LICENSE"),
    ("README.md", "README.md"),
)


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="latin-1")


def extract_regex(path: Path, pattern: str, label: str) -> str:
    text = read_text(path)
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        fail(f"could not read {label} from {path}")
    return match.group(1).strip()


def is_prerelease(version: str) -> bool:
    return bool(re.search(r"[A-Za-z]", version or ""))


def validate_versions(root: Path, channel: str, version: str) -> None:
    if channel == "stable" and is_prerelease(version):
        fail(f"stable release version must not be a prerelease: {version}")
    if channel == "beta" and not is_prerelease(version):
        fail(f"beta release version must be a prerelease: {version}")

    code_version = extract_regex(
        root / "src/YAL_hoppiehelper.cpp",
        r'kPluginVersion\s*=\s*"([^"]+)"',
        "plugin version",
    )
    readme_version = extract_regex(
        root / "README.md",
        r"Current plugin version:\s*([^\s]+)",
        "README version",
    )

    expected = {
        "src/YAL_hoppiehelper.cpp": code_version,
        "README.md": readme_version,
    }
    for name, actual in expected.items():
        if actual != version:
            fail(f"{name} has version {actual}, expected {version}")


def sha256_stream(stream) -> str:
    digest = hashlib.sha256()
    while True:
        chunk = stream.read(1024 * 1024)
        if not chunk:
            break
        digest.update(chunk)
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return sha256_stream(stream)


def is_safe_package_path(value: object) -> bool:
    if (
        not isinstance(value, str)
        or not value
        or "\\" in value
        or ":" in value
        or "\0" in value
        or "*" in value
        or "?" in value
        or value.startswith("/")
    ):
        return False
    return all(part not in ("", ".", "..") for part in value.split("/"))


def collect_files(root: Path) -> list[tuple[Path, str]]:
    files: list[tuple[Path, str]] = []
    seen_targets: set[str] = set()
    for source_name, target_name in PACKAGE_MAP:
        source = root / source_name
        if not source.is_file():
            fail(f"required release file missing: {source_name}")
        if not is_safe_package_path(target_name):
            fail(f"unsafe release target path: {target_name}")
        normalized = target_name.casefold()
        if normalized in seen_targets:
            fail(f"case-insensitive target collision: {target_name}")
        seen_targets.add(normalized)
        files.append((source, target_name))
    return sorted(files, key=lambda item: item[1].lower())


def build_file_entries(zip_path: Path, package_files: list[tuple[Path, str]]) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    with zipfile.ZipFile(zip_path, "r") as archive:
        for _, rel in package_files:
            member_name = f"{PACKAGE_ROOT}/{rel}"
            info = archive.getinfo(member_name)
            with archive.open(info, "r") as stream:
                digest = sha256_stream(stream)
            entries.append({
                "path": rel,
                "size": info.file_size,
                "sha256": digest,
            })
    return entries


def verify_release_package(zip_path: Path, json_manifest_path: Path, channel: str, version: str) -> None:
    try:
        manifest = json.loads(json_manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"could not read JSON manifest {json_manifest_path}: {exc}")
    if not isinstance(manifest, dict):
        fail("JSON manifest root must be an object")

    expected_identity = {
        "schemaVersion": 1,
        "packageId": PACKAGE_ID,
        "packageVersion": version,
        "releaseTag": f"{RELEASE_TAG_PREFIX}{version}",
        "channel": channel,
        "repository": REPOSITORY,
        "installScope": INSTALL_SCOPE,
        "targetPath": TARGET_PATH,
        "supportedProducts": list(SUPPORTED_PRODUCTS),
        "restartRequired": True,
        "protectedPaths": list(PROTECTED_PATHS),
    }
    for key, expected in expected_identity.items():
        if manifest.get(key) != expected:
            fail(f"JSON manifest {key} is {manifest.get(key)!r}, expected {expected!r}")

    archive_metadata = manifest.get("archive")
    if not isinstance(archive_metadata, dict):
        fail("JSON manifest archive metadata is missing")
    if archive_metadata.get("fileName") != zip_path.name:
        fail("JSON manifest archive file name does not match the release ZIP")
    if archive_metadata.get("rootPath") != PACKAGE_ROOT:
        fail(f"JSON manifest archive root must be {PACKAGE_ROOT}")
    if archive_metadata.get("size") != zip_path.stat().st_size:
        fail("JSON manifest archive size does not match the release ZIP")
    if archive_metadata.get("sha256") != sha256_file(zip_path):
        fail("JSON manifest archive SHA-256 does not match the release ZIP")

    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries:
        fail("JSON manifest contains no package files")

    entries_by_path: dict[str, dict[str, object]] = {}
    normalized_paths: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            fail("JSON manifest contains an invalid file entry")
        path = entry.get("path")
        size = entry.get("size")
        digest = entry.get("sha256")
        if not is_safe_package_path(path):
            fail(f"JSON manifest contains an unsafe package path: {path!r}")
        if path in entries_by_path:
            fail(f"JSON manifest contains a duplicate package path: {path}")
        normalized_path = path.casefold()
        if normalized_path in normalized_paths:
            fail(f"JSON manifest contains a case-insensitive package path collision: {path}")
        if type(size) is not int or size < 0:
            fail(f"JSON manifest contains an invalid size for {path}")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            fail(f"JSON manifest contains an invalid SHA-256 for {path}")
        entries_by_path[path] = entry
        normalized_paths.add(normalized_path)

    try:
        with zipfile.ZipFile(zip_path, "r") as archive:
            file_infos = [info for info in archive.infolist() if not info.is_dir()]
            member_names = [info.filename for info in file_infos]
            if len(member_names) != len(set(member_names)):
                fail("release ZIP contains duplicate file entries")

            expected_members = {f"{PACKAGE_ROOT}/{path}" for path in entries_by_path}
            if set(member_names) != expected_members:
                fail("release ZIP and JSON manifest file lists do not match")

            for path, entry in entries_by_path.items():
                info = archive.getinfo(f"{PACKAGE_ROOT}/{path}")
                if info.file_size != entry["size"]:
                    fail(f"release ZIP size does not match JSON manifest for {path}")
                with archive.open(info, "r") as stream:
                    digest = sha256_stream(stream)
                if digest != entry["sha256"]:
                    fail(f"release ZIP SHA-256 does not match JSON manifest for {path}")
    except zipfile.BadZipFile as exc:
        fail(f"release ZIP is invalid: {exc}")


def write_checksums(path: Path, assets: list[Path]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        for asset in assets:
            stream.write(f"{sha256_file(asset)}  {asset.name}\n")


def build_release_assets(root: Path, output_dir: Path, channel: str, version: str) -> tuple[Path, Path, Path, Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    zip_path = output_dir / f"{ARCHIVE_PREFIX}-{version}.zip"
    manifest_path = output_dir / f"{ARCHIVE_PREFIX}-{version}-manifest.txt"
    json_manifest_path = output_dir / f"{ARCHIVE_PREFIX}-{version}-manifest.json"
    checksum_path = output_dir / f"{ARCHIVE_PREFIX}-{version}-checksums.txt"
    package_files = collect_files(root)

    for output_path in (zip_path, manifest_path, json_manifest_path, checksum_path):
        if output_path.exists():
            output_path.unlink()

    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True) as archive:
        for source, rel in package_files:
            archive.write(source, f"{PACKAGE_ROOT}/{rel}")

    with manifest_path.open("w", encoding="utf-8", newline="\n") as manifest:
        manifest.write(f"YAL HoppieHelper release package {version}\n")
        manifest.write(f"root={PACKAGE_ROOT}\n")
        manifest.write(f"file_count={len(package_files)}\n\n")
        for _, rel in package_files:
            manifest.write(rel + "\n")

    json_manifest = {
        "schemaVersion": 1,
        "packageId": PACKAGE_ID,
        "packageVersion": version,
        "releaseTag": f"{RELEASE_TAG_PREFIX}{version}",
        "channel": channel,
        "repository": REPOSITORY,
        "installScope": INSTALL_SCOPE,
        "targetPath": TARGET_PATH,
        "supportedProducts": list(SUPPORTED_PRODUCTS),
        "restartRequired": True,
        "protectedPaths": list(PROTECTED_PATHS),
        "archive": {
            "fileName": zip_path.name,
            "rootPath": PACKAGE_ROOT,
            "size": zip_path.stat().st_size,
            "sha256": sha256_file(zip_path),
        },
        "files": build_file_entries(zip_path, package_files),
    }
    json_manifest_path.write_text(
        json.dumps(json_manifest, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )
    verify_release_package(zip_path, json_manifest_path, channel, version)
    write_checksums(checksum_path, [zip_path, manifest_path, json_manifest_path])

    return zip_path, manifest_path, json_manifest_path, checksum_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=("stable", "beta"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--root", default=".")
    parser.add_argument("--output-dir", default="dist")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    output_dir = Path(args.output_dir).resolve()
    validate_versions(root, args.channel, args.version)
    zip_path, manifest_path, json_manifest_path, checksum_path = build_release_assets(
        root,
        output_dir,
        args.channel,
        args.version,
    )

    print(f"zip={zip_path}")
    print(f"manifest={manifest_path}")
    print(f"json_manifest={json_manifest_path}")
    print(f"checksums={checksum_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
