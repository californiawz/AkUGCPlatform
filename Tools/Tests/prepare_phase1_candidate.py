"""Prepare frozen Phase 1 candidate archives without claiming acceptance.

The generated archives are inputs to phase1_external_acceptance.py init. This
tool verifies a clean source commit and archive structure, but it cannot prove
that prebuilt binaries were produced from that commit.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import uuid
import zipfile

from phase1_external_acceptance import (
    capture_source_state,
    inspect_creator_package,
    inspect_pe_file,
    inspect_runtime_package,
    sha256_file,
    write_json,
)


SCHEMA_VERSION = 1
HEX_40 = re.compile(r"^[0-9a-fA-F]{40}$")
CREATOR_TOP_LEVEL = (
    "AkUGCPlatform.uproject",
    "Config",
    "Content",
    "Plugins",
    "Source",
    "Lua",
    "Binaries",
)
CREATOR_EXCLUDED_DIRECTORIES = {
    ".git",
    ".idea",
    ".vs",
    ".vscode",
    "__pycache__",
    "DerivedDataCache",
    "Intermediate",
    "Saved",
}
CREATOR_EXCLUDED_SUFFIXES = {
    ".exp",
    ".lib",
    ".obj",
    ".opensdf",
    ".pdb",
    ".pyc",
    ".sarif",
    ".sdf",
    ".suo",
}
PRIVATE_KEY_SUFFIXES = {".key", ".p12", ".pfx"}
PRIVATE_KEY_NAMES = {
    "id_rsa",
    "id_dsa",
    "id_ecdsa",
    "id_ed25519",
    "private.key",
    "publisher-identity.json",
}
CREATOR_MODULE_ARCHIVE_PATH = (
    "AkUGCPlatform/Plugins/AkUGCEditor/Binaries/Win64/"
    "UnrealEditor-AkUGCEditor.dll"
)
CREATOR_MODULES_ARCHIVE_PATH = (
    "AkUGCPlatform/Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor.modules"
)
RUNTIME_TARGET_ARCHIVE_PATHS = {
    "game_package": "AkUGCPlatform/Binaries/Win64/AkUGCPlatform.target",
    "client_package": "AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.target",
    "server_package": "AkUGCPlatform/Binaries/Win64/AkUGCPlatformServer.target",
}
RUNTIME_PROJECT_ARCHIVE_PATH = "AkUGCPlatform/AkUGCPlatform.uproject"
RUNTIME_EXCLUDED_DIRECTORIES = {"Saved"}


def is_private_key_name(name: str) -> bool:
    folded = name.casefold()
    return (
        folded in PRIVATE_KEY_NAMES
        or any(folded.startswith(private_name + ".")
               for private_name in PRIVATE_KEY_NAMES)
    )


def is_link_or_junction(path: Path) -> bool:
    is_junction = getattr(path, "is_junction", None)
    return path.is_symlink() or bool(is_junction and is_junction())


def reject_private_key(path: Path) -> None:
    lowered = path.name.casefold()
    if path.suffix.casefold() in PRIVATE_KEY_SUFFIXES or is_private_key_name(lowered):
        raise ValueError(f"candidate input contains a private-key file: {path}")
    if path.suffix.casefold() == ".pem" and path.stat().st_size <= 1024 * 1024:
        payload = path.read_bytes()
        if b"-----BEGIN " in payload and b"PRIVATE KEY-----" in payload:
            raise ValueError(f"candidate input contains private-key material: {path}")


def collect_tree(
        root: Path,
        archive_root: str,
        excluded_directories: set[str] | None = None,
        excluded_suffixes: set[str] | None = None) -> dict[str, Path]:
    root = root.resolve(strict=True)
    if not root.is_dir() or is_link_or_junction(root):
        raise ValueError(f"candidate root must be a regular directory: {root}")
    excluded_directories = excluded_directories or set()
    excluded_suffixes = excluded_suffixes or set()
    folded_excluded_directories = {
        name.casefold() for name in excluded_directories
    }
    result: dict[str, Path] = {}
    folded: set[str] = set()
    for directory, names, files in os.walk(root, followlinks=False):
        parent = Path(directory)
        kept_directories = []
        for name in names:
            child = parent / name
            if name.casefold() in folded_excluded_directories:
                continue
            if is_link_or_junction(child):
                raise ValueError(f"candidate input contains a linked directory: {child}")
            kept_directories.append(name)
        names[:] = kept_directories
        for name in files:
            source = parent / name
            if is_link_or_junction(source) or not source.is_file():
                raise ValueError(f"candidate input contains a linked or non-regular file: {source}")
            if source.suffix.casefold() in excluded_suffixes:
                continue
            reject_private_key(source)
            relative = source.relative_to(root).as_posix()
            archive_name = str(PurePosixPath(archive_root) / relative)
            key = archive_name.casefold()
            if key in folded:
                raise ValueError(f"candidate input has a case-colliding path: {archive_name}")
            folded.add(key)
            result[archive_name] = source
    return result


def inject_entry(
        entries: dict[str, Path],
        source: Path,
        archive_name: str,
        label: str) -> None:
    source = source.resolve(strict=True)
    if not source.is_file() or is_link_or_junction(source):
        raise ValueError(f"{label} must be a regular file: {source}")
    reject_private_key(source)
    folded_entries = {name.casefold(): name for name in entries}
    existing_name = folded_entries.get(archive_name.casefold())
    existing = entries.get(existing_name) if existing_name else None
    if existing and sha256_file(existing) != sha256_file(source):
        raise ValueError(f"{label} conflicts with canonical path: {archive_name}")
    if existing_name and existing_name != archive_name:
        del entries[existing_name]
    entries[archive_name] = source


def collect_creator(
        project_root: Path,
        creator_module: Path,
        creator_modules_manifest: Path) -> dict[str, Path]:
    project_root = project_root.resolve(strict=True)
    entries: dict[str, Path] = {}
    for name in CREATOR_TOP_LEVEL:
        source = project_root / name
        if not source.exists():
            if name in {
                    "AkUGCPlatform.uproject", "Config", "Content", "Plugins", "Lua"}:
                raise ValueError(f"Creator source is missing required path: {source}")
            continue
        if source.is_file():
            reject_private_key(source)
            entries[f"AkUGCPlatform/{name}"] = source
            continue
        entries.update(collect_tree(
            source,
            f"AkUGCPlatform/{name}",
            CREATOR_EXCLUDED_DIRECTORIES,
            CREATOR_EXCLUDED_SUFFIXES,
        ))
    for source, archive_name in (
            (creator_module, CREATOR_MODULE_ARCHIVE_PATH),
            (creator_modules_manifest, CREATOR_MODULES_ARCHIVE_PATH)):
        inject_entry(entries, source, archive_name, "Creator module input")
    return entries


def collect_runtime(
        root: Path,
        project_descriptor: Path,
        target_receipt: Path,
        role: str) -> dict[str, Path]:
    entries = collect_tree(
        root, "AkUGCPlatform",
        excluded_directories=RUNTIME_EXCLUDED_DIRECTORIES)
    inject_entry(
        entries, project_descriptor, RUNTIME_PROJECT_ARCHIVE_PATH,
        "runtime project descriptor")
    inject_entry(
        entries, target_receipt, RUNTIME_TARGET_ARCHIVE_PATHS[role],
        f"{role} target receipt")
    return entries


def write_archive(path: Path, entries: dict[str, Path]) -> None:
    if not entries:
        raise ValueError(f"refusing to write an empty archive: {path}")
    snapshots = {
        source: (source.stat().st_size, source.stat().st_mtime_ns)
        for source in set(entries.values())
    }
    with zipfile.ZipFile(
            path, "x", compression=zipfile.ZIP_STORED, allowZip64=True,
            strict_timestamps=False) as archive:
        for archive_name, source in sorted(entries.items()):
            archive.write(source, archive_name)
    for source, expected in snapshots.items():
        current = (source.stat().st_size, source.stat().st_mtime_ns)
        if current != expected:
            raise ValueError(f"candidate input changed while archiving: {source}")


def artifact_record(path: Path, sha256: str, file_count: int | None = None) -> dict:
    result = {
        "file": path.name,
        "sha256": sha256,
        "size_bytes": path.stat().st_size,
    }
    if file_count is not None:
        result["file_count"] = file_count
    return result


def prepare_candidate(
        project_root: Path,
        source_commit: str,
        creator_module: Path,
        creator_modules_manifest: Path,
        game_root: Path,
        game_target: Path,
        client_root: Path,
        client_target: Path,
        server_root: Path,
        server_target: Path,
        output: Path) -> Path:
    project_root = project_root.resolve(strict=True)
    source_commit = source_commit.lower()
    if HEX_40.fullmatch(source_commit) is None:
        raise ValueError("source_commit must be a full 40-digit hexadecimal commit")
    actual_commit, source_clean = capture_source_state(project_root)
    if actual_commit != source_commit or not source_clean:
        raise ValueError(
            "project_root must be at source_commit with no tracked or non-ignored "
            "untracked changes")
    output = output.resolve()
    if output.exists():
        raise ValueError(f"refusing to overwrite candidate output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + f".preparing-{uuid.uuid4().hex}")
    baseline = temporary / "baseline"
    try:
        baseline.mkdir(parents=True)
        creator_entries = collect_creator(
            project_root, creator_module, creator_modules_manifest)
        runtime_inputs = {
            "game_package": (game_root, game_target),
            "client_package": (client_root, client_target),
            "server_package": (server_root, server_target),
        }
        runtime_entries = {
            role: collect_runtime(
                Path(root), project_root / "AkUGCPlatform.uproject",
                Path(target), role)
            for role, (root, target) in runtime_inputs.items()
        }
        creator_build = baseline / "CreatorBuild.dll"
        shutil.copy2(Path(creator_module).resolve(strict=True), creator_build)
        creator_package = baseline / "CreatorPackage.zip"
        game_package = baseline / "GamePackage.zip"
        client_package = baseline / "ClientPackage.zip"
        server_package = baseline / "ServerPackage.zip"
        write_archive(creator_package, creator_entries)
        for path, role in (
                (game_package, "game_package"),
                (client_package, "client_package"),
                (server_package, "server_package")):
            write_archive(path, runtime_entries[role])

        creator_build_sha256 = inspect_pe_file(
            creator_build, True, "CreatorBuild.dll")
        creator_info = inspect_creator_package(creator_package)
        if creator_info["creator_module_sha256"] != creator_build_sha256:
            raise ValueError("Creator archive module differs from CreatorBuild.dll")
        runtime_info = {
            role: inspect_runtime_package(path, role)
            for role, path in (
                ("game_package", game_package),
                ("client_package", client_package),
                ("server_package", server_package),
            )
        }
        final_commit, final_clean = capture_source_state(project_root)
        if final_commit != source_commit or not final_clean:
            raise ValueError("project_root changed while candidate archives were prepared")
        report = {
            "schema_version": SCHEMA_VERSION,
            "kind": "phase1_candidate_archive_preparation",
            "created_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "source_commit": source_commit,
            "source_worktree_clean": True,
            "local_rehearsal": True,
            "production_acceptance_evidence": False,
            "artifacts": {
                "creator_build": artifact_record(
                    creator_build, creator_build_sha256),
                "creator_package": artifact_record(
                    creator_package, creator_info["archive_sha256"],
                    len(creator_entries)),
                "game_package": artifact_record(
                    game_package, runtime_info["game_package"]["archive_sha256"],
                    len(runtime_entries["game_package"])),
                "client_package": artifact_record(
                    client_package, runtime_info["client_package"]["archive_sha256"],
                    len(runtime_entries["client_package"])),
                "server_package": artifact_record(
                    server_package, runtime_info["server_package"]["archive_sha256"],
                    len(runtime_entries["server_package"])),
            },
            "creator_build_id": creator_info["creator_build_id"],
            "excluded_runtime_directories": sorted(RUNTIME_EXCLUDED_DIRECTORIES),
            "boundary": (
                "This report proves archive construction, static package structure, a clean "
                "declared source worktree, and artifact hashes. It does not prove that the "
                "prebuilt binaries were produced from source_commit, does not execute human "
                "acceptance, and must not be used as external acceptance evidence."
            ),
        }
        write_json(temporary / "candidate-preparation.json", report)
        temporary.rename(output)
        return output / "candidate-preparation.json"
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--creator-module", required=True, type=Path)
    parser.add_argument("--creator-modules-manifest", required=True, type=Path)
    parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--game-target", required=True, type=Path)
    parser.add_argument("--client-root", required=True, type=Path)
    parser.add_argument("--client-target", required=True, type=Path)
    parser.add_argument("--server-root", required=True, type=Path)
    parser.add_argument("--server-target", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_arguments(argv)
    report = prepare_candidate(
        args.project_root,
        args.source_commit,
        args.creator_module,
        args.creator_modules_manifest,
        args.game_root,
        args.game_target,
        args.client_root,
        args.client_target,
        args.server_root,
        args.server_target,
        args.output,
    )
    print(report)
    print("phase1_candidate_archives_prepared= True")
    print("production_acceptance_evidence= False")
    return 0


if __name__ == "__main__":
    sys.exit(main())
