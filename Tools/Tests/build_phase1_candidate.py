"""Build a Phase 1 Win64 candidate from a clean source commit.

This records local source-to-output provenance for an isolated worktree. It is
not a reproducible-build proof or a production build-service attestation.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import time
import uuid

from phase1_external_acceptance import (
    capture_source_state,
    inspect_pe_file,
    sha256_file,
    strict_json_loads,
    validate_target_receipt,
    validate_unreal_pak_footer,
    write_json,
)
from verify_rooms import run_cook_checked


SCHEMA_VERSION = 1
GENERATED_ROOTS = ("Binaries", "DerivedDataCache", "Intermediate", "Saved")
GENERATED_BUILD_DIRECTORIES = (
    "Build/Windows",
    "Build/WindowsClient",
    "Build/WindowsServer",
)
ROLES = {
    "game": {
        "target": "AkUGCPlatform",
        "target_type": "Game",
        "stage_platform": "Windows",
        "executable": "AkUGCPlatform.exe",
        "uat_flags": ("-clientconfig=Development",),
    },
    "client": {
        "target": "AkUGCPlatformClient",
        "target_type": "Client",
        "stage_platform": "WindowsClient",
        "executable": "AkUGCPlatformClient.exe",
        "uat_flags": (
            "-client",
            "-noserver",
            "-clientconfig=Development",
        ),
    },
    "server": {
        "target": "AkUGCPlatformServer",
        "target_type": "Server",
        "stage_platform": "WindowsServer",
        "executable": "AkUGCPlatformServer.exe",
        "uat_flags": (
            "-server",
            "-noclient",
            "-serverplatform=Win64",
            "-serverconfig=Development",
        ),
    },
}


class BuildStepFailure(RuntimeError):
    def __init__(self, record: dict):
        super().__init__(record.get("error", record["name"]))
        self.record = record


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def is_link_or_junction(path: Path) -> bool:
    is_junction = getattr(path, "is_junction", None)
    return path.is_symlink() or bool(is_junction and is_junction())


def stable_file_record(path: Path, relative_to: Path | None = None) -> dict:
    path = path.resolve(strict=True)
    if not path.is_file() or is_link_or_junction(path):
        raise ValueError(f"expected a regular build artifact: {path}")
    before = path.stat()
    digest = sha256_file(path)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f"build artifact changed while hashing: {path}")
    name = (
        path.relative_to(relative_to.resolve()).as_posix()
        if relative_to is not None else path.name
    )
    return {
        "file": name,
        "sha256": digest,
        "size_bytes": after.st_size,
    }


def read_json_file(path: Path, label: str, limit: int) -> dict:
    path = path.resolve(strict=True)
    if not path.is_file() or is_link_or_junction(path):
        raise ValueError(f"{label} must be a regular file: {path}")
    size = path.stat().st_size
    if not 0 < size <= limit:
        raise ValueError(f"{label} has an invalid size: {size}")
    try:
        value = strict_json_loads(path.read_text(encoding="utf-8-sig"))
    except (UnicodeError, json.JSONDecodeError, ValueError, RecursionError) as error:
        raise ValueError(f"{label} is not valid strict JSON") from error
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a JSON object")
    return value


def engine_fingerprint(engine_root: Path, luac: Path) -> dict:
    engine_root = engine_root.resolve(strict=True)
    tools = {
        "build_batch": engine_root / "Engine/Build/BatchFiles/Build.bat",
        "uat_batch": engine_root / "Engine/Build/BatchFiles/RunUAT.bat",
        "editor_commandlet": (
            engine_root / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"),
        "unreal_build_tool": (
            engine_root
            / "Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll"),
        "lua_compiler": luac,
    }
    version_path = engine_root / "Engine/Build/Build.version"
    version = read_json_file(version_path, "Unreal Build.version", 1024 * 1024)
    return {
        "engine_root": str(engine_root),
        "build_version": version,
        "files": {
            name: stable_file_record(path)
            for name, path in tools.items()
        },
        "build_version_file": stable_file_record(version_path),
    }


def assert_fresh_project(project_root: Path) -> None:
    generated = [project_root / name for name in GENERATED_ROOTS]
    generated.extend(
        project_root / name for name in GENERATED_BUILD_DIRECTORIES)
    generated.append(project_root / "Content/Lua")
    generated.append(
        project_root
        / "Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor-AkUGCEditor.dll")
    existing = [str(path) for path in generated if path.exists()]
    if existing:
        raise ValueError(
            "fresh build requires an isolated worktree without generated outputs: "
            + ", ".join(existing))


def validate_project_inputs(project_root: Path) -> None:
    project = project_root / "AkUGCPlatform.uproject"
    if not project.is_file() or is_link_or_junction(project):
        raise ValueError(f"missing regular project descriptor: {project}")
    config = project_root / "Config/DefaultGame.ini"
    if not config.is_file() or is_link_or_junction(config):
        raise ValueError(f"missing regular packaging config: {config}")
    section = ""
    lua_staged = False
    for raw_line in config.read_text(
            encoding="utf-8-sig", errors="strict").splitlines():
        line = raw_line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line.casefold()
            continue
        normalized = "".join(line.split()).casefold()
        if (
                section == "[/script/unrealed.projectpackagingsettings]"
                and normalized
                == '+directoriestoalwaysstageasufs=(path="lua")'):
            lua_staged = True
    if not lua_staged:
        raise ValueError(
            "DefaultGame.ini must stage Content/Lua as UFS for packaged runtime")


def build_commands(
        project_root: Path,
        engine_root: Path,
        luac: Path) -> tuple[list[dict], list[dict]]:
    project = project_root / "AkUGCPlatform.uproject"
    lua_root = project_root / "Lua"
    lua_sources = sorted(lua_root.rglob("*.lua"))
    if not lua_sources:
        raise ValueError(f"no Lua sources found under {lua_root}")
    lua_steps = []
    lua_records = []
    for index, source in enumerate(lua_sources, start=1):
        if not source.is_file() or is_link_or_junction(source):
            raise ValueError(f"Lua source must be a regular file: {source}")
        relative = source.relative_to(lua_root)
        output = project_root / "Content/Lua" / relative.with_suffix(".luac")
        lua_steps.append({
            "name": f"compile_lua_{index:03d}",
            "command": [str(luac), "-o", str(output), str(source)],
            "log": f"compile_lua_{index:03d}.log",
            "timeout_seconds": 300,
            "expected_output": output,
        })
        lua_records.append({
            "source": source,
            "output": output,
        })

    build_batch = engine_root / "Engine/Build/BatchFiles/Build.bat"
    uat_batch = engine_root / "Engine/Build/BatchFiles/RunUAT.bat"
    steps = [*lua_steps, {
        "name": "build_editor",
        "command": [
            str(build_batch),
            "AkUGCPlatformEditor",
            "Win64",
            "Development",
            str(project),
            "-WaitMutex",
            "-NoHotReloadFromIDE",
        ],
        "log": "build_editor.log",
        "timeout_seconds": 7200,
    }]
    common_uat = [
        "BuildCookRun",
        f"-project={project}",
        "-noP4",
        "-unattended",
        "-utf8output",
        "-platform=Win64",
    ]
    for role, spec in ROLES.items():
        steps.append({
            "name": f"package_{role}",
            "command": [
                str(uat_batch),
                *common_uat,
                f"-target={spec['target']}",
                "-build",
                "-cook",
                "-stage",
                "-pak",
                "-package",
                *spec["uat_flags"],
            ],
            "log": f"package_{role}.log",
            "timeout_seconds": 7200,
        })
    return steps, lua_records


def default_runner(
        command: list[str],
        cwd: Path,
        log_path: Path,
        timeout_seconds: int) -> None:
    previous = Path.cwd()
    try:
        os.chdir(cwd)
        run_cook_checked(command, log_path, timeout_seconds)
    finally:
        os.chdir(previous)


def execute_step(
        step: dict,
        project_root: Path,
        logs: Path,
        runner) -> dict:
    started = utc_now()
    monotonic_start = time.monotonic()
    log_path = logs / step["log"]
    log_path.parent.mkdir(parents=True, exist_ok=True)
    expected_output = step.get("expected_output")
    if expected_output is not None:
        expected_output.parent.mkdir(parents=True, exist_ok=True)
    error = None
    try:
        runner(
            step["command"],
            project_root,
            log_path,
            step["timeout_seconds"],
        )
    except Exception as caught:
        error = f"{type(caught).__name__}: {caught}"
    result = {
        "name": step["name"],
        "command": [str(value) for value in step["command"]],
        "started_at_utc": started,
        "finished_at_utc": utc_now(),
        "duration_seconds": round(time.monotonic() - monotonic_start, 3),
        "timeout_seconds": step["timeout_seconds"],
        "exit_code": 0 if error is None else None,
        "log": f"logs/{step['log']}",
    }
    if log_path.is_file():
        result["log_sha256"] = sha256_file(log_path)
        result["log_size_bytes"] = log_path.stat().st_size
    if error is not None:
        result["error"] = error
        raise BuildStepFailure(result)
    if expected_output is not None and not expected_output.is_file():
        result["error"] = f"missing expected output: {expected_output}"
        raise BuildStepFailure(result)
    return result


def validate_pak(path: Path) -> None:
    path = path.resolve(strict=True)
    size = path.stat().st_size
    tail_size = min(size, 64 * 1024)
    with path.open("rb") as stream:
        stream.seek(size - tail_size)
        tail = stream.read(tail_size)
    validate_unreal_pak_footer(
        tail, size - tail_size, size, str(path))


def tree_manifest(root: Path) -> dict:
    root = root.resolve(strict=True)
    if not root.is_dir() or is_link_or_junction(root):
        raise ValueError(f"staged root must be a regular directory: {root}")
    files = []
    for directory, names, filenames in os.walk(root, followlinks=False):
        parent = Path(directory)
        for name in names:
            child = parent / name
            if is_link_or_junction(child):
                raise ValueError(f"staged root contains a linked directory: {child}")
        for name in filenames:
            path = parent / name
            files.append(stable_file_record(path, root))
    files.sort(key=lambda item: item["file"])
    if not files:
        raise ValueError(f"staged root is empty: {root}")
    aggregate = hashlib.sha256(json.dumps(
        files, ensure_ascii=True, separators=(",", ":")).encode("utf-8")).hexdigest()
    return {
        "root": str(root),
        "file_count": len(files),
        "total_bytes": sum(item["size_bytes"] for item in files),
        "manifest_sha256": aggregate,
        "files": files,
    }


def validate_build_outputs(project_root: Path) -> dict:
    creator_directory = (
        project_root / "Plugins/AkUGCEditor/Binaries/Win64")
    creator_module = creator_directory / "UnrealEditor-AkUGCEditor.dll"
    creator_modules = creator_directory / "UnrealEditor.modules"
    creator_sha256 = inspect_pe_file(
        creator_module, True, "fresh Creator module")
    modules = read_json_file(
        creator_modules, "fresh Creator modules manifest", 4 * 1024 * 1024)
    try:
        build_id = str(uuid.UUID(modules["BuildId"]))
    except (KeyError, TypeError, ValueError, AttributeError) as error:
        raise ValueError("fresh Creator BuildId is invalid") from error
    if (
            not isinstance(modules.get("Modules"), dict)
            or modules["Modules"].get("AkUGCEditor")
            != "UnrealEditor-AkUGCEditor.dll"):
        raise ValueError("fresh Creator modules manifest does not bind AkUGCEditor")

    roles = {}
    for role, spec in ROLES.items():
        target = (
            project_root / "Binaries/Win64"
            / f"{spec['target']}.target")
        target_value = read_json_file(
            target, f"{role} target receipt", 4 * 1024 * 1024)
        validate_target_receipt(
            target_value, spec["executable"], spec["target_type"])
        stage = (
            project_root / "Saved/StagedBuilds"
            / spec["stage_platform"] / "AkUGCPlatform")
        if (stage / "Saved").exists():
            raise ValueError(f"fresh {role} staged root contains runtime Saved data")
        executable = stage / "Binaries/Win64" / spec["executable"]
        executable_sha256 = inspect_pe_file(
            executable, False, f"fresh {role} executable")
        paks = sorted((stage / "Content/Paks").glob("*.pak"))
        if not paks:
            raise ValueError(f"fresh {role} staged root has no Pak")
        for pak in paks:
            validate_pak(pak)
        roles[role] = {
            "target_receipt": stable_file_record(target, project_root),
            "target_build_id": str(uuid.UUID(target_value["Version"]["BuildId"])),
            "executable": {
                **stable_file_record(executable, project_root),
                "sha256": executable_sha256,
            },
            "paks": [
                stable_file_record(pak, project_root)
                for pak in paks
            ],
            "staged_tree": tree_manifest(stage),
        }
    return {
        "creator": {
            "module": {
                **stable_file_record(creator_module, project_root),
                "sha256": creator_sha256,
            },
            "modules_manifest": stable_file_record(
                creator_modules, project_root),
            "build_id": build_id,
        },
        "roles": roles,
    }


def clean_generated_lua(lua_records: list[dict], project_root: Path) -> None:
    generated_root = project_root / "Content/Lua"
    if not generated_root.exists():
        return
    expected = {
        record["output"].resolve()
        for record in lua_records
    }
    actual = {
        path.resolve()
        for path in generated_root.rglob("*")
        if path.is_file()
    }
    if not actual.issubset(expected):
        raise ValueError(
            "refusing to remove Content/Lua because it contains unexpected files")
    shutil.rmtree(generated_root)


def clean_generated_build_directories(project_root: Path) -> None:
    for relative in GENERATED_BUILD_DIRECTORIES:
        path = project_root / relative
        if not path.exists():
            continue
        if not path.is_dir() or is_link_or_junction(path):
            raise ValueError(
                f"refusing to remove unexpected generated build path: {path}")
        shutil.rmtree(path)


def run_fresh_build(
        project_root: Path,
        source_commit: str,
        engine_root: Path,
        luac: Path,
        output: Path,
        runner=default_runner) -> Path:
    project_root = project_root.resolve(strict=True)
    engine_root = engine_root.resolve(strict=True)
    luac = luac.resolve(strict=True)
    output = output.resolve()
    if output.exists():
        raise ValueError(f"refusing to overwrite build provenance output: {output}")
    if output == project_root or project_root in output.parents:
        raise ValueError("build provenance output must be outside project_root")
    actual_commit, clean = capture_source_state(project_root)
    source_commit = source_commit.lower()
    if actual_commit != source_commit or not clean:
        raise ValueError("project_root must be a clean worktree at source_commit")
    validate_project_inputs(project_root)
    assert_fresh_project(project_root)
    fingerprint = engine_fingerprint(engine_root, luac)
    steps, lua_records = build_commands(project_root, engine_root, luac)

    output.mkdir(parents=True)
    logs = output / "logs"
    logs.mkdir()
    started = utc_now()
    step_results = []
    failure = None
    try:
        for step in steps:
            try:
                step_results.append(
                    execute_step(step, project_root, logs, runner))
            except BuildStepFailure as error:
                step_results.append(error.record)
                raise
        lua_outputs = [
            {
                "source": stable_file_record(record["source"], project_root),
                "compiled": stable_file_record(record["output"], project_root),
            }
            for record in lua_records
        ]
        artifacts = validate_build_outputs(project_root)
    except Exception as error:
        failure = f"{type(error).__name__}: {error}"
        raise
    finally:
        cleanup_error = None
        try:
            clean_generated_lua(lua_records, project_root)
            clean_generated_build_directories(project_root)
        except Exception as error:
            cleanup_error = f"{type(error).__name__}: {error}"
        final_commit, final_clean = capture_source_state(project_root)
        if (
                failure is not None or cleanup_error is not None
                or final_commit != source_commit or not final_clean):
            write_json(output / "fresh-build-failure.json", {
                "schema_version": SCHEMA_VERSION,
                "kind": "phase1_fresh_build_failure",
                "started_at_utc": started,
                "finished_at_utc": utc_now(),
                "source_commit": source_commit,
                "failure": failure,
                "cleanup_error": cleanup_error,
                "final_source_commit": final_commit,
                "final_source_clean": final_clean,
                "steps": step_results,
                "production_build_attestation": False,
            })
            if failure is None:
                raise RuntimeError(
                    cleanup_error or "source worktree changed during fresh build")

    report = {
        "schema_version": SCHEMA_VERSION,
        "kind": "phase1_fresh_local_build_provenance",
        "started_at_utc": started,
        "finished_at_utc": utc_now(),
        "source_commit": source_commit,
        "source_worktree_clean_before": True,
        "source_worktree_clean_after": True,
        "fresh_project_outputs": True,
        "local_build_provenance": True,
        "production_build_attestation": False,
        "reproducible_build_verified": False,
        "environment": {
            "platform": platform.platform(),
            "python_version": platform.python_version(),
            "shared_engine_and_caches": True,
        },
        "engine": fingerprint,
        "lua_outputs": lua_outputs,
        "steps": step_results,
        "artifacts": artifacts,
        "boundary": (
            "This report binds a clean source commit, local tool fingerprints, commands, "
            "logs, and output hashes from one isolated build. Shared engine artifacts and "
            "caches were used. It is not a reproducible-build comparison, a hermetic build, "
            "a production build-service attestation, or external acceptance evidence."
        ),
    }
    path = output / "fresh-build-provenance.json"
    write_json(path, report)
    return path


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--luac", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_arguments(argv)
    report = run_fresh_build(
        args.project_root,
        args.source_commit,
        args.engine,
        args.luac,
        args.output,
    )
    print(report)
    print("fresh_local_build_provenance= True")
    print("production_build_attestation= False")
    return 0


if __name__ == "__main__":
    sys.exit(main())
