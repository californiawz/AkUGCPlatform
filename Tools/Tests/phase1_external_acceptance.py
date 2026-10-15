"""Generate and validate Phase 1 human/cross-device acceptance evidence.

This gate validates evidence completeness and internal consistency. Human and
physical-device claims remain explicit attestations; software cannot prove them.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import subprocess
import sys
import uuid
import zipfile

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


SCHEMA_VERSION = 1
PRODUCT_SCHEMA_VERSION = 6
SUPPORTED_CAPABILITIES = {"logic", "ruleset", "world.spawn", "rules.wave"}
HEX_32 = re.compile(r"^[0-9a-fA-F]{32}$")
HEX_40 = re.compile(r"^[0-9a-fA-F]{40}$")
HEX_64 = re.compile(r"^[0-9a-fA-F]{64}$")
RESILIENCE_CASES = {"client_leave", "network_loss", "host_terminated", "reconnect"}
SENSITIVE_IDENTITY_NAMES = {
    "private.key",
    "publisher-identity.json",
}
PAK_MAGIC = b"\xe1\x12\x6f\x5a"
PAK_VERSION_LATEST = 12
PAK_COMPRESSION_METHOD_BYTES = 32 * 5


def is_sensitive_identity_name(name: str) -> bool:
    folded = name.casefold()
    return (
        folded in SENSITIVE_IDENTITY_NAMES
        or folded.endswith(".akugcrecovery")
        or ".akugcrecovery." in folded
        or any(folded.startswith(identity + ".")
               for identity in SENSITIVE_IDENTITY_NAMES)
    )


def sha256_stream(stream) -> str:
    digest = hashlib.sha256()
    stream.seek(0)
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(block)
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return sha256_stream(stream)


def capture_source_state(root: Path) -> tuple[str, bool]:
    try:
        commit = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
            capture_output=True, text=True, encoding="utf-8").stdout.strip().lower()
        source_status = subprocess.run(
            [
                "git", "-C", str(root), "status", "--porcelain=v1",
                "--untracked-files=all", "--ignore-submodules=none",
            ],
            check=True, capture_output=True, text=True, encoding="utf-8").stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError("Cannot verify source commit and source worktree state") from error
    return commit, not source_status.strip()


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())


def strict_json_loads(text: str):
    def reject_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON field: {key}")
            result[key] = value
        return result

    def reject_constant(value):
        raise ValueError(f"non-finite JSON number: {value}")

    def finite_float(value):
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError(f"non-finite JSON number: {value}")
        return parsed

    return json.loads(
        text, object_pairs_hook=reject_duplicates,
        parse_constant=reject_constant, parse_float=finite_float)


def nested_strings(value):
    stack = [value]
    while stack:
        current = stack.pop()
        if isinstance(current, str):
            yield current
        elif isinstance(current, dict):
            stack.extend(current.values())
        elif isinstance(current, list):
            stack.extend(current)


def normalized_archive(package: zipfile.ZipFile, label: str):
    entries = package.infolist()
    if not 1 <= len(entries) <= 100_000:
        raise ValueError(f"{label} has an invalid entry count")
    names = []
    for entry in entries:
        name = entry.filename.replace("\\", "/")
        path = PurePosixPath(name)
        if (not name or "\x00" in name or name.startswith("/")
                or re.match(r"^[A-Za-z]:", name) is not None
                or ".." in path.parts):
            raise ValueError(f"{label} contains an unsafe path")
        names.append(name)
    folded = [name.casefold() for name in names]
    if len(folded) != len(set(folded)):
        raise ValueError(f"{label} contains duplicate or case-colliding paths")
    for entry, name in zip(entries, names):
        if name.endswith("/"):
            continue
        pure = PurePosixPath(name)
        lowered = pure.name.casefold()
        if (pure.suffix.casefold() in {
                    ".akugcrecovery", ".key", ".p12", ".pfx"}
                or lowered in {"id_rsa", "id_dsa", "id_ecdsa", "id_ed25519"}
                or is_sensitive_identity_name(lowered)):
            raise ValueError(f"{label} contains a private-key file: {name}")
        if pure.suffix.casefold() == ".pem" and entry.file_size <= 1024 * 1024:
            payload = package.read(entry)
            if b"-----BEGIN " in payload and b"PRIVATE KEY-----" in payload:
                raise ValueError(f"{label} contains private-key material: {name}")
    return entries, names


def archive_json(package: zipfile.ZipFile, entry: zipfile.ZipInfo, label: str, limit: int):
    if not 0 < entry.file_size <= limit:
        raise ValueError(f"{label} size is invalid")
    try:
        return strict_json_loads(package.read(entry).decode("utf-8-sig"))
    except (
            UnicodeError, json.JSONDecodeError, ValueError, RuntimeError,
            RecursionError) as error:
        raise ValueError(f"{label} is not valid strict JSON") from error


def validate_pe_prefix(payload: bytes, file_size: int, expect_dll: bool, label: str) -> None:
    if file_size < 4096 or len(payload) < 0x200 or payload[:2] != b"MZ":
        raise ValueError(f"{label} is not a non-trivial Windows PE image")
    pe_offset = int.from_bytes(payload[0x3c:0x40], "little")
    if pe_offset < 0x40 or pe_offset + 26 > len(payload):
        raise ValueError(f"{label} has an invalid PE header offset")
    if payload[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError(f"{label} is missing the PE signature")
    machine = int.from_bytes(payload[pe_offset + 4:pe_offset + 6], "little")
    section_count = int.from_bytes(payload[pe_offset + 6:pe_offset + 8], "little")
    optional_size = int.from_bytes(payload[pe_offset + 20:pe_offset + 22], "little")
    characteristics = int.from_bytes(payload[pe_offset + 22:pe_offset + 24], "little")
    optional_offset = pe_offset + 24
    section_table = optional_offset + optional_size
    if (machine != 0x8664 or not 1 <= section_count <= 96 or optional_size < 112
            or optional_offset + optional_size > len(payload)
            or section_table + section_count * 40 > len(payload)
            or int.from_bytes(payload[optional_offset:optional_offset + 2], "little") != 0x20B
            or not characteristics & 0x0002
            or bool(characteristics & 0x2000) != expect_dll):
        raise ValueError(f"{label} is not the expected Win64 {'DLL' if expect_dll else 'EXE'}")
    entry_point = int.from_bytes(
        payload[optional_offset + 16:optional_offset + 20], "little")
    section_alignment = int.from_bytes(
        payload[optional_offset + 32:optional_offset + 36], "little")
    file_alignment = int.from_bytes(
        payload[optional_offset + 36:optional_offset + 40], "little")
    image_size = int.from_bytes(
        payload[optional_offset + 56:optional_offset + 60], "little")
    headers_size = int.from_bytes(
        payload[optional_offset + 60:optional_offset + 64], "little")
    subsystem = int.from_bytes(
        payload[optional_offset + 68:optional_offset + 70], "little")
    directory_count = int.from_bytes(
        payload[optional_offset + 108:optional_offset + 112], "little")
    if (entry_point == 0 or not 512 <= file_alignment <= 65536
            or file_alignment & (file_alignment - 1)
            or section_alignment < file_alignment
            or section_alignment & (section_alignment - 1)
            or image_size < section_alignment or image_size % section_alignment
            or not section_table + section_count * 40 <= headers_size <= file_size
            or headers_size % file_alignment
            or subsystem not in {2, 3}
            or directory_count < 16):
        raise ValueError(f"{label} has invalid PE32+ image layout")
    executable_entry = False
    for index in range(section_count):
        offset = section_table + index * 40
        name = payload[offset:offset + 8].rstrip(b"\0")
        virtual_size = int.from_bytes(payload[offset + 8:offset + 12], "little")
        virtual_address = int.from_bytes(payload[offset + 12:offset + 16], "little")
        raw_size = int.from_bytes(payload[offset + 16:offset + 20], "little")
        raw_offset = int.from_bytes(payload[offset + 20:offset + 24], "little")
        section_flags = int.from_bytes(payload[offset + 36:offset + 40], "little")
        span = max(virtual_size, raw_size)
        if (not name or virtual_size == 0 or virtual_address % section_alignment
                or raw_size % file_alignment or raw_offset % file_alignment
                or raw_offset + raw_size > file_size
                or virtual_address + span > image_size):
            raise ValueError(f"{label} has an invalid PE section table")
        if (section_flags & 0x20000000 and section_flags & 0x00000020
                and virtual_address <= entry_point < virtual_address + span):
            executable_entry = True
    if not executable_entry:
        raise ValueError(f"{label} entry point is not inside executable code")


def validate_zip_pe(
        package: zipfile.ZipFile, entry: zipfile.ZipInfo, expect_dll: bool, label: str) -> None:
    with package.open(entry) as stream:
        prefix = stream.read(min(entry.file_size, 1024 * 1024))
    validate_pe_prefix(prefix, entry.file_size, expect_dll, label)


def inspect_pe_file(path: Path, expect_dll: bool, label: str) -> str:
    try:
        with path.open("rb") as stream:
            file_size = os.fstat(stream.fileno()).st_size
            digest = sha256_stream(stream)
            stream.seek(0)
            prefix = stream.read(min(file_size, 1024 * 1024))
            validate_pe_prefix(prefix, file_size, expect_dll, label)
            if sha256_stream(stream) != digest:
                raise ValueError(f"{label} changed while being inspected")
            return digest
    except OSError as error:
        raise ValueError(f"{label} could not be inspected") from error


def validate_unreal_pak_footer(
        tail: bytes, tail_offset: int, file_size: int, label: str) -> None:
    if file_size < 4096:
        raise ValueError(f"{label} is too small to be a packaged Unreal Pak")
    cursor = 0
    while True:
        cursor = tail.find(PAK_MAGIC, cursor)
        if cursor < 0:
            break
        footer_prefix_size = 17
        magic_to_end = 44 + PAK_COMPRESSION_METHOD_BYTES
        if (cursor >= footer_prefix_size
                and cursor + magic_to_end == len(tail)
                and tail[cursor - 1] in {0, 1}):
            version = int.from_bytes(tail[cursor + 4:cursor + 8], "little", signed=True)
            index_offset = int.from_bytes(
                tail[cursor + 8:cursor + 16], "little", signed=True)
            index_size = int.from_bytes(
                tail[cursor + 16:cursor + 24], "little", signed=True)
            index_hash = tail[cursor + 24:cursor + 44]
            method_bytes = tail[cursor + 44:cursor + magic_to_end]
            footer_offset = tail_offset + cursor - footer_prefix_size
            methods_valid = all(
                b"\0" in method_bytes[offset:offset + 32]
                for offset in range(0, PAK_COMPRESSION_METHOD_BYTES, 32))
            if (version == PAK_VERSION_LATEST and index_offset >= 0 and index_size > 0
                    and index_offset + index_size <= footer_offset
                    and any(index_hash) and methods_valid):
                return
        cursor += 1
    raise ValueError(f"{label} is missing a bounded Unreal Pak info footer")


def validate_unreal_pak(
        package: zipfile.ZipFile, entry: zipfile.ZipInfo, label: str) -> None:
    tail_offset = max(0, entry.file_size - 64 * 1024)
    with package.open(entry) as stream:
        stream.seek(tail_offset)
        tail = stream.read(64 * 1024)
    validate_unreal_pak_footer(tail, tail_offset, entry.file_size, label)


def validate_target_receipt(value, executable: str, target_type: str) -> None:
    target_name = executable.removesuffix(".exe")
    if not isinstance(value, dict):
        raise ValueError("runtime target receipt must be an object")
    version = value.get("Version")
    products = value.get("BuildProducts")
    try:
        build_id = uuid.UUID(version.get("BuildId")) if isinstance(version, dict) else None
    except (AttributeError, TypeError, ValueError) as error:
        raise ValueError("runtime target receipt BuildId is invalid") from error
    expected_suffix = f"/Binaries/Win64/{executable}"
    launch = str(value.get("Launch", "")).replace("\\", "/")
    has_product = isinstance(products, list) and any(
        isinstance(product, dict)
        and product.get("Type") == "Executable"
        and str(product.get("Path", "")).replace("\\", "/").endswith(expected_suffix)
        for product in products
    )
    if (value.get("TargetName") != target_name
            or value.get("TargetType") != target_type
            or value.get("Platform") != "Win64"
            or value.get("Configuration") not in {"Development", "Shipping"}
            or value.get("Architecture") != "x64"
            or value.get("IsTestTarget") is not False
            or not launch.endswith(expected_suffix)
            or not has_product
            or build_id is None or build_id.int == 0):
        raise ValueError(
            f"runtime target receipt does not describe {target_name} Win64 {target_type}")


def inspect_creator_package(path: Path) -> dict:
    try:
        with path.open("rb") as archive_stream:
            archive_digest = sha256_stream(archive_stream)
            archive_stream.seek(0)
            with zipfile.ZipFile(archive_stream) as package:
                entries, normalized = normalized_archive(package, "Creator package")
                entry_by_name = dict(zip(normalized, entries))
                projects = [
                    name for name in normalized
                    if PurePosixPath(name).name == "AkUGCPlatform.uproject"]
                if len(projects) != 1:
                    raise ValueError(
                        "Creator package must contain exactly one AkUGCPlatform.uproject")
                project_root = projects[0][:-len("AkUGCPlatform.uproject")]

                def project_path(relative: str) -> str:
                    return project_root + relative

                module_name = project_path(
                    "Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor-AkUGCEditor.dll")
                modules_name = project_path(
                    "Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor.modules")
                if normalized.count(module_name) != 1:
                    raise ValueError(
                        "Creator package must contain exactly one canonical AkUGCEditor module")
                if modules_name not in normalized:
                    raise ValueError("Creator package is missing the AkUGCEditor BuildId manifest")
                if project_path("Plugins/AkUGCEditor/AkUGCEditor.uplugin") not in normalized:
                    raise ValueError("Creator package is missing the AkUGCEditor plugin descriptor")
                if not any(
                        name.startswith(project_path("Config/")) and not name.endswith("/")
                        for name in normalized):
                    raise ValueError("Creator package is missing project Config")
                if not any(name.startswith(project_path("Content/")) and not name.endswith("/")
                           for name in normalized):
                    raise ValueError("Creator package is missing project Content")
                project = archive_json(
                    package, entry_by_name[projects[0]], "Creator project descriptor",
                    1024 * 1024)
                plugins = project.get("Plugins") if isinstance(project, dict) else None
                if (not isinstance(project, dict)
                        or type(project.get("FileVersion")) is not int
                        or project["FileVersion"] <= 0
                        or not isinstance(plugins, list) or not any(
                        isinstance(plugin, dict)
                        and plugin.get("Name") == "AkUGCEditor"
                        and plugin.get("Enabled") is True for plugin in plugins)):
                    raise ValueError("Creator project does not enable AkUGCEditor")
                plugin_name = project_path("Plugins/AkUGCEditor/AkUGCEditor.uplugin")
                plugin = archive_json(
                    package, entry_by_name[plugin_name], "AkUGCEditor plugin descriptor",
                    1024 * 1024)
                plugin_modules = plugin.get("Modules") if isinstance(plugin, dict) else None
                if (not isinstance(plugin, dict)
                        or type(plugin.get("FileVersion")) is not int
                        or plugin["FileVersion"] <= 0
                        or not isinstance(plugin_modules, list) or not any(
                        isinstance(module, dict)
                        and module.get("Name") == "AkUGCEditor"
                        and module.get("Type") == "Editor" for module in plugin_modules)):
                    raise ValueError(
                        "Creator plugin descriptor does not declare AkUGCEditor")
                module_entry = entry_by_name[module_name]
                if not 0 < module_entry.file_size <= 512 * 1024 * 1024:
                    raise ValueError("Creator module entry size is invalid")
                validate_zip_pe(package, module_entry, True, "Creator AkUGCEditor module")
                modules_entry = entry_by_name[modules_name]
                modules = archive_json(
                    package, modules_entry, "Creator BuildId manifest", 1024 * 1024)
                if (not isinstance(modules, dict)
                        or not isinstance(modules.get("BuildId"), str)
                        or not isinstance(modules.get("Modules"), dict)
                        or modules["Modules"].get("AkUGCEditor")
                            != "UnrealEditor-AkUGCEditor.dll"):
                    raise ValueError(
                        "Creator BuildId manifest does not map the AkUGCEditor module")
                build_id = uuid.UUID(modules["BuildId"])
                if build_id.int == 0:
                    raise ValueError("Creator BuildId must be non-zero")
                digest = hashlib.sha256()
                with package.open(module_entry) as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(block)
            if sha256_stream(archive_stream) != archive_digest:
                raise ValueError("Creator package changed while being inspected")
            return {
                "archive_sha256": archive_digest,
                "creator_module_sha256": digest.hexdigest(),
                "creator_build_id": str(build_id),
            }
    except (
            OSError, UnicodeError, zipfile.BadZipFile, RuntimeError, ValueError,
            json.JSONDecodeError, RecursionError) as error:
        raise ValueError(f"invalid Creator package: {error}") from error


def inspect_runtime_package(path: Path, role: str) -> dict:
    executable, target_type = {
        "game_package": ("AkUGCPlatform.exe", "Game"),
        "client_package": ("AkUGCPlatformClient.exe", "Client"),
        "server_package": ("AkUGCPlatformServer.exe", "Server"),
    }[role]
    receipt = executable.removesuffix(".exe") + ".target"
    try:
        with path.open("rb") as archive_stream:
            archive_digest = sha256_stream(archive_stream)
            archive_stream.seek(0)
            with zipfile.ZipFile(archive_stream) as package:
                entries, names = normalized_archive(package, role)
                entry_by_name = dict(zip(names, entries))
                executable_suffix = "Binaries/Win64/" + executable
                executable_names = [
                    name for name in names if name.endswith(executable_suffix)]
                if len(executable_names) != 1:
                    raise ValueError(f"expected exactly one {executable}")
                executable_name = executable_names[0]
                project_root = executable_name[:-len(executable_suffix)]
                receipt_name = project_root + "Binaries/Win64/" + receipt
                project_name = project_root + "AkUGCPlatform.uproject"
                if receipt_name not in entry_by_name:
                    raise ValueError(f"missing canonical {receipt}")
                if project_name not in entry_by_name:
                    raise ValueError("missing canonical AkUGCPlatform.uproject")
                project = archive_json(
                    package, entry_by_name[project_name], f"{role} project descriptor",
                    1024 * 1024)
                if (not isinstance(project, dict)
                        or type(project.get("FileVersion")) is not int
                        or project["FileVersion"] <= 0):
                    raise ValueError(f"{role} project descriptor is invalid")
                validate_zip_pe(
                    package, entry_by_name[executable_name], False, f"{role} executable")
                target = archive_json(
                    package, entry_by_name[receipt_name], f"{role} target receipt",
                    4 * 1024 * 1024)
                validate_target_receipt(target, executable, target_type)
                pak_entries = [
                    entry_by_name[name] for name in names
                    if name.startswith(project_root + "Content/Paks/")
                    and name.casefold().endswith(".pak")]
                if not pak_entries:
                    raise ValueError("missing packaged Content/Paks")
                for entry in pak_entries:
                    validate_unreal_pak(package, entry, f"{role} {entry.filename}")
            if sha256_stream(archive_stream) != archive_digest:
                raise ValueError(f"{role} changed while being inspected")
            return {"archive_sha256": archive_digest}
    except (
            OSError, UnicodeError, zipfile.BadZipFile, RuntimeError, ValueError,
            json.JSONDecodeError, RecursionError) as error:
        raise ValueError(f"invalid {role}: {error}") from error


def parse_work_package(raw: bytes, trusted_public_key: str) -> dict:
    if len(raw) > 16 * 1024 * 1024:
        raise ValueError("work package exceeds 16 MiB")
    try:
        root = strict_json_loads(raw.decode("utf-8-sig"))
    except (UnicodeError, json.JSONDecodeError, ValueError, RecursionError) as error:
        raise ValueError(f"invalid work package JSON: {type(error).__name__}") from error
    if not isinstance(root, dict) or set(root) != {"manifest", "document", "program", "signature"}:
        raise ValueError("work package must contain only manifest/document/program/signature")
    manifest, signature = root["manifest"], root["signature"]
    manifest_keys = {
        "releaseId", "schemaVersion", "projectId", "templateId", "capabilities",
        "assetDependencies", "contentHash",
    }
    if not isinstance(manifest, dict) or set(manifest) != manifest_keys:
        raise ValueError("work package manifest fields are invalid")
    if not isinstance(root["document"], dict) or not isinstance(root["program"], dict):
        raise ValueError("work package document/program must be objects")
    if not isinstance(signature, dict) or set(signature) != {"algorithm", "publicKey", "signature"}:
        raise ValueError("work package signature fields are invalid")
    normalized_trusted_key = (
        trusted_public_key.strip().lower() if isinstance(trusted_public_key, str) else "")
    try:
        release_id = uuid.UUID(manifest["releaseId"])
        project_id = uuid.UUID(manifest["projectId"])
    except (ValueError, AttributeError, TypeError) as error:
        raise ValueError("work package manifest GUID is invalid") from error
    if (type(manifest["schemaVersion"]) is not int
            or manifest["schemaVersion"] != PRODUCT_SCHEMA_VERSION
            or release_id.int == 0 or project_id.int == 0
            or not isinstance(manifest["templateId"], str) or not manifest["templateId"]
            or not isinstance(manifest["capabilities"], list)
            or not all(isinstance(item, str) and item.lower() in SUPPORTED_CAPABILITIES
                       for item in manifest["capabilities"])
            or not isinstance(manifest["assetDependencies"], list)
            or not all(isinstance(item, str) for item in manifest["assetDependencies"])
            or not isinstance(manifest["contentHash"], str)
            or re.fullmatch(r"[0-9a-f]{64}", manifest["contentHash"]) is None):
        raise ValueError("work package manifest value types are invalid")
    if (signature.get("algorithm") != "ed25519"
            or not isinstance(signature.get("publicKey"), str)
            or re.fullmatch(r"[0-9a-f]{64}", signature["publicKey"]) is None
            or not isinstance(signature.get("signature"), str)
            or re.fullmatch(r"[0-9a-fA-F]{128}", signature["signature"]) is None
            or re.fullmatch(r"[0-9a-f]{64}", normalized_trusted_key) is None
            or signature["publicKey"] != normalized_trusted_key):
        raise ValueError("work package signature metadata or trusted public key is invalid")
    canonical_manifest = {
        "assetdependencies": manifest["assetDependencies"],
        "capabilities": [item.lower() for item in manifest["capabilities"]],
        "contenthash": manifest["contentHash"],
        "projectid": str(project_id).lower(),
        "releaseid": str(release_id).lower(),
        "schemaversion": manifest["schemaVersion"],
        "templateid": manifest["templateId"].lower(),
    }
    message = json.dumps(
        canonical_manifest, ensure_ascii=True, separators=(",", ":"), sort_keys=True).encode("utf-8")
    try:
        Ed25519PublicKey.from_public_bytes(bytes.fromhex(normalized_trusted_key)).verify(
            bytes.fromhex(signature["signature"]), message)
    except (ValueError, InvalidSignature) as error:
        raise ValueError("work package Ed25519 signature is invalid") from error
    try:
        payload = json.dumps(
            {"document": root["document"], "program": root["program"]},
            ensure_ascii=True, separators=(",", ":"), sort_keys=True).encode("utf-8")
    except (TypeError, ValueError, RecursionError) as error:
        raise ValueError("work package payload nesting is invalid") from error
    return {
        "release_id": release_id.hex,
        "content_hash": manifest["contentHash"].lower(),
        "payload_fingerprint": hashlib.sha256(payload).hexdigest(),
        "manifest_fingerprint": hashlib.sha256(json.dumps(
            manifest, ensure_ascii=True, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")).hexdigest(),
        "signature_fingerprint": hashlib.sha256(json.dumps(
            signature, ensure_ascii=True, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")).hexdigest(),
        "public_key": signature["publicKey"],
    }


def participant_template() -> dict:
    return {
        "device": "",
        "package_role": "client",
        "package_sha256": "",
        "release_id": "",
        "content_hash": "",
        "result": "",
        "team_score": None,
        "base_health": None,
        "resources": None,
        "log": "",
        "screenshot": "",
    }


def seed_run_template(index: int) -> dict:
    player_count = 4 if index == 9 else 2
    host_mode = "listen" if index % 2 == 0 else "dedicated"
    participants = [participant_template() for _ in range(player_count)]
    if host_mode == "listen":
        participants[0]["package_role"] = "game"
    return {
        "seed_id": f"seed-{index + 1}",
        "release_id": "",
        "trusted_public_key": "",
        "share_file": "",
        "share_sha256": "",
        "transferred_between_devices": False,
        "independent_key_channel_verified": False,
        "tampered_copy_rejected": False,
        "tampered_copy_file": "",
        "tampered_copy_sha256": "",
        "tamper_rejection_evidence": "",
        "host_mode": host_mode,
        "host_device": "",
        "host_package_sha256": "",
        "host_observation": ({
            "participant_index": 0,
        } if host_mode == "listen" else {
            "device": "",
            "package_role": "server",
            "package_sha256": "",
            "release_id": "",
            "content_hash": "",
            "result": "",
            "team_score": None,
            "base_health": None,
            "resources": None,
            "log": "",
        }),
        "participants": participants,
        "observer_alias": "",
        "observed_at_utc": "",
        "observer_attested": False,
        "notes": "",
    }


def resilience_run_template(case: str) -> dict:
    host_mode = "listen" if case == "host_terminated" else "dedicated"
    host_role = "game" if host_mode == "listen" else "server"
    return {
        "case": case,
        "observer_alias": "",
        "observed_at_utc": "",
        "host_mode": host_mode,
        "host_device": "",
        "host_package_sha256": "",
        "fault_device": "",
        "recovery_device": "",
        "fault_evidence": "",
        "recovery_evidence": "",
        "fault_action_observed": False,
        "net_driver_cleared": False,
        "roster_cleared": False,
        "session_cleared": False,
        "recovered_to_clean_state": False,
        "followup_play_succeeded": False,
        "new_process_used": False,
        "device_reports": [
            {
                "device": "", "package_role": host_role, "package_sha256": "",
                "before_process_instance": "", "after_process_instance": "", "log": "",
            },
            {
                "device": "", "package_role": "client", "package_sha256": "",
                "before_process_instance": "", "after_process_instance": "", "log": "",
            },
        ],
        "observer_attested": False,
        "notes": "",
    }


def make_template(source_commit: str = "", session_nonce: str | None = None) -> dict:
    return {
        "schema_version": SCHEMA_VERSION,
        "session_nonce": session_nonce or secrets.token_hex(16),
        "evidence_lock": "evidence-lock.json",
        "baseline": {
            "source_commit": source_commit,
            "creator_build_file": "",
            "creator_build_sha256": "",
            "creator_package_file": "",
            "creator_package_sha256": "",
            "game_package_file": "",
            "game_package_sha256": "",
            "client_package_file": "",
            "client_package_sha256": "",
            "server_package_file": "",
            "server_package_sha256": "",
        },
        "physical_slider": {
            "tester_alias": "",
            "observer_alias": "",
            "observed_at_utc": "",
            "evidence_capture_consent": False,
            "used_physical_mouse": False,
            "automated_slider_motion_disabled": False,
            "manual_slider_report": "",
            "evidence_files": [],
            "observer_attested": False,
            "notes": "",
        },
        "usability": {
            "tester_alias": "",
            "observer_alias": "",
            "tester_is_non_programmer": False,
            "evidence_capture_consent": False,
            "started_at_utc": "",
            "finished_at_utc": "",
            "used_logic_graph": None,
            "created_project": False,
            "saved_project": False,
            "published_work": False,
            "exported_share": False,
            "recipient_imported_and_played": False,
            "independent_key_channel_verified": False,
            "creator_device": "",
            "recipient_device": "",
            "creator_package_sha256": "",
            "share_file": "",
            "share_sha256": "",
            "trusted_public_key": "",
            "evidence_files": [],
            "observer_attested": False,
            "notes": "",
        },
        "cross_device": {
            "seed_runs": [seed_run_template(index) for index in range(10)],
            "resilience_runs": [
                resilience_run_template(case) for case in sorted(RESILIENCE_CASES)
            ],
        },
        "final_attestation": {
            "coordinator_alias": "",
            "devices_are_physical_and_distinct": False,
            "real_network_used": False,
            "real_file_transfer_used": False,
            "same_machine_processes_not_counted": False,
            "claims_are_human_attested_not_software_proven": False,
            "evidence_unmodified": False,
            "signed_at_utc": "",
            "attestation_file": "",
            "attestation_sha256": "",
            "signed_off": False,
        },
    }


class Validator:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.diagnostics: list[str] = []
        self.evidence_hashes: dict[str, str] = {}
        self.evidence_bytes: dict[str, bytes] = {}
        self.evidence_roles: dict[str, str] = {}
        self.digest_roles: dict[str, set[str]] = {}

    def check(self, condition: bool, message: str) -> bool:
        if not condition:
            self.diagnostics.append(message)
        return condition

    def exact_keys(self, value, expected: set[str], label: str) -> bool:
        if not isinstance(value, dict):
            self.diagnostics.append(f"{label} must be an object")
            return False
        missing = sorted(expected - set(value))
        unknown = sorted(set(value) - expected)
        if missing:
            self.diagnostics.append(f"{label} missing fields: {', '.join(missing)}")
        if unknown:
            self.diagnostics.append(f"{label} has unknown fields: {', '.join(unknown)}")
        return not missing and not unknown

    def nonempty(self, value, label: str) -> bool:
        return self.check(isinstance(value, str) and bool(value.strip()), f"{label} must be non-empty text")

    def truth(self, value, label: str) -> bool:
        return self.check(value is True, f"{label} must be explicitly true")

    def hash_value(self, value, label: str, pattern=HEX_64) -> bool:
        return self.check(isinstance(value, str) and pattern.fullmatch(value) is not None,
                          f"{label} must be a lowercase-or-uppercase hexadecimal digest")

    def timestamp(self, value, label: str):
        if not isinstance(value, str) or not value:
            self.diagnostics.append(f"{label} must be an ISO-8601 timestamp with timezone")
            return None
        try:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError:
            self.diagnostics.append(f"{label} must be an ISO-8601 timestamp with timezone")
            return None
        if parsed.tzinfo is None or parsed.utcoffset() is None:
            self.diagnostics.append(f"{label} must include a timezone")
            return None
        return parsed

    def evidence_file(self, value, label: str, suffix: str | None = None, role: str = "generic"):
        if not isinstance(value, str) or not value.strip():
            self.diagnostics.append(f"{label} must reference a relative evidence file")
            return None
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts:
            self.diagnostics.append(f"{label} escapes the evidence directory")
            return None
        candidate = self.root / relative
        try:
            resolved = candidate.resolve(strict=True)
        except (FileNotFoundError, OSError):
            self.diagnostics.append(f"{label} does not exist: {value}")
            return None
        if not resolved.is_relative_to(self.root) or candidate.is_symlink():
            self.diagnostics.append(f"{label} must be a regular in-directory file")
            return None
        lowered = resolved.name.lower()
        if (resolved.suffix.lower() in {".akugcrecovery", ".key"}
                or is_sensitive_identity_name(lowered)
                or lowered == "fixture-tls.key"):
            self.diagnostics.append(f"{label} must not collect a private key")
            return None
        if not resolved.is_file() or resolved.stat().st_size <= 0:
            self.diagnostics.append(f"{label} must be a non-empty regular file")
            return None
        if suffix and resolved.suffix.lower() != suffix.lower():
            self.diagnostics.append(f"{label} must have the {suffix} extension")
            return None
        normalized = relative.as_posix()
        if normalized in self.evidence_hashes:
            digest = self.evidence_hashes[normalized]
        else:
            structured = resolved.suffix.lower() in {".json", ".akugc"}
            if structured and resolved.stat().st_size > 16 * 1024 * 1024:
                self.diagnostics.append(f"{label} structured evidence exceeds 16 MiB")
                return None
            try:
                if structured:
                    data = resolved.read_bytes()
                    self.evidence_bytes[normalized] = data
                    digest = hashlib.sha256(data).hexdigest()
                else:
                    digest = sha256_file(resolved)
            except OSError:
                self.diagnostics.append(f"{label} could not be read")
                return None
        previous_role = self.evidence_roles.get(normalized)
        if previous_role:
            self.diagnostics.append(
                f"{label} reuses evidence already assigned to role {previous_role}")
        previous_digest_roles = self.digest_roles.get(digest, set())
        if previous_digest_roles:
            self.diagnostics.append(
                f"{label} reuses evidence content assigned to roles {', '.join(sorted(previous_digest_roles))}")
        self.evidence_roles[normalized] = role
        self.digest_roles.setdefault(digest, set()).add(role)
        self.evidence_hashes[normalized] = digest
        return resolved

    def bytes_for(self, value) -> bytes | None:
        if not isinstance(value, str):
            return None
        return self.evidence_bytes.get(Path(value).as_posix())

    def verify_unchanged(self) -> None:
        for relative, expected in self.evidence_hashes.items():
            try:
                current = sha256_file(self.root / relative)
            except OSError:
                current = ""
            self.check(current == expected, f"evidence changed while validating: {relative}")

    def unique_evidence_content(self, values, label: str) -> None:
        if not isinstance(values, list):
            return
        normalized = [Path(item).as_posix() for item in values if isinstance(item, str)]
        digests = [self.evidence_hashes.get(item) for item in normalized]
        self.check(len(digests) == len(values) and all(digests)
                   and len(digests) == len(set(digests)),
                   f"{label} must not reuse identical evidence content")


TOP_KEYS = {
    "schema_version", "session_nonce", "evidence_lock", "baseline", "physical_slider", "usability",
    "cross_device", "final_attestation",
}
BASELINE_KEYS = {
    "source_commit", "creator_build_file", "creator_build_sha256", "creator_package_file",
    "creator_package_sha256", "game_package_file", "game_package_sha256",
    "client_package_file", "client_package_sha256", "server_package_file", "server_package_sha256",
}
PHYSICAL_SLIDER_KEYS = {
    "tester_alias", "observer_alias", "observed_at_utc", "evidence_capture_consent",
    "used_physical_mouse",
    "automated_slider_motion_disabled", "manual_slider_report", "evidence_files",
    "observer_attested", "notes",
}
USABILITY_KEYS = {
    "tester_alias", "observer_alias", "tester_is_non_programmer", "evidence_capture_consent",
    "started_at_utc", "finished_at_utc", "used_logic_graph", "created_project", "saved_project",
    "published_work", "exported_share",
    "recipient_imported_and_played", "independent_key_channel_verified", "creator_device",
    "recipient_device", "creator_package_sha256", "share_file", "share_sha256",
    "trusted_public_key", "evidence_files", "observer_attested", "notes",
}
CROSS_DEVICE_KEYS = {"seed_runs", "resilience_runs"}
SEED_KEYS = {
    "seed_id", "release_id", "share_file", "share_sha256", "transferred_between_devices",
    "independent_key_channel_verified", "trusted_public_key", "tampered_copy_rejected",
    "tampered_copy_file", "tampered_copy_sha256", "tamper_rejection_evidence",
    "host_device", "host_mode",
    "host_package_sha256", "host_observation", "participants", "observer_alias",
    "observed_at_utc", "observer_attested", "notes",
}
PARTICIPANT_KEYS = {
    "device", "package_role", "package_sha256", "release_id", "content_hash", "result",
    "team_score", "base_health", "resources", "log", "screenshot",
}
HOST_OBSERVATION_KEYS = PARTICIPANT_KEYS - {"screenshot"}
LISTEN_HOST_OBSERVATION_KEYS = {"participant_index"}
RESILIENCE_KEYS = {
    "case", "observer_alias", "observed_at_utc", "host_mode", "host_device", "host_package_sha256",
    "fault_device", "recovery_device", "fault_evidence", "recovery_evidence",
    "fault_action_observed", "net_driver_cleared", "roster_cleared", "session_cleared",
    "recovered_to_clean_state", "followup_play_succeeded", "new_process_used",
    "device_reports", "observer_attested", "notes",
}
RESILIENCE_DEVICE_REPORT_KEYS = {
    "device", "package_role", "package_sha256", "before_process_instance",
    "after_process_instance", "log",
}
ATTESTATION_KEYS = {
    "coordinator_alias", "devices_are_physical_and_distinct", "real_network_used",
    "real_file_transfer_used", "same_machine_processes_not_counted",
    "claims_are_human_attested_not_software_proven", "evidence_unmodified", "signed_at_utc",
    "attestation_file", "attestation_sha256", "signed_off",
}


def observed_slider_evidence(value) -> bool:
    return (
        isinstance(value, dict)
        and value.get("target") == "wave_count"
        and value.get("target_verified") is True
        and isinstance(value.get("capture_type"), str)
        and value["capture_type"].startswith("SSpinBox")
        and value.get("start_waves") == 3
        and value.get("captured_waves") == 2
        and value.get("released_waves") == 2
        and type(value.get("nonzero_delta_events")) is int
        and value["nonzero_delta_events"] > 0
        and type(value.get("high_precision_delta_events")) is int
        and value["high_precision_delta_events"] > 0
        and value.get("released") is True
        and value.get("keyboard_value_input") is False
    )


def validate_physical_slider(value, session_nonce: str, baseline: dict, validator: Validator) -> dict:
    metrics = {"physical_slider_observed": False, "physical_slider_observed_at_utc": None}
    if not validator.exact_keys(value, PHYSICAL_SLIDER_KEYS, "physical_slider"):
        value = value if isinstance(value, dict) else {}
    validator.nonempty(value.get("tester_alias"), "physical_slider.tester_alias")
    validator.nonempty(value.get("observer_alias"), "physical_slider.observer_alias")
    observed_at = validator.timestamp(
        value.get("observed_at_utc"), "physical_slider.observed_at_utc")
    if observed_at:
        metrics["physical_slider_observed_at_utc"] = observed_at.isoformat()
    for field in (
        "evidence_capture_consent", "used_physical_mouse", "automated_slider_motion_disabled",
        "observer_attested",
    ):
        validator.truth(value.get(field), f"physical_slider.{field}")
    validator.check(isinstance(value.get("notes"), str), "physical_slider.notes must be text")
    report_path = validator.evidence_file(value.get("manual_slider_report"),
                                          "physical_slider.manual_slider_report", ".json", "slider_report")
    report_valid = False
    if report_path:
        try:
            report = strict_json_loads(
                (validator.bytes_for(value.get("manual_slider_report")) or b"").decode("utf-8-sig"))
        except (UnicodeError, json.JSONDecodeError, ValueError, RecursionError):
            validator.diagnostics.append("physical_slider.manual_slider_report is not valid JSON")
        else:
            if not isinstance(report, dict):
                validator.diagnostics.append(
                    "physical_slider.manual_slider_report must contain a JSON object")
                report = {}
            expected_tags = [
                "new", "slider", "draft", "saved-draft", "parameters-applied",
                "parameters-undo", "parameters-redo", "saved-applied", "reloaded", "discarded",
            ]
            checkpoints = report.get("checkpoints")
            actions = report.get("actions")
            checkpoint_valid = (
                isinstance(checkpoints, list)
                and [item.get("tag") for item in checkpoints if isinstance(item, dict)] == expected_tags
                and all(item.get("passed") is True for item in checkpoints if isinstance(item, dict))
                and all(
                    isinstance(item, dict)
                    and isinstance(item.get("acceptance_nonce"), str)
                    and isinstance(session_nonce, str)
                    and item["acceptance_nonce"].lower() == session_nonce.lower()
                    and isinstance(item.get("creator_build_sha256"), str)
                    and isinstance(baseline.get("creator_build_sha256"), str)
                    and item["creator_build_sha256"].lower()
                        == baseline["creator_build_sha256"].lower()
                    for item in checkpoints
                )
                and len(checkpoints) == len(expected_tags)
                and observed_slider_evidence(checkpoints[1].get("slider_evidence"))
            )
            actions_valid = (
                isinstance(actions, list)
                and any(isinstance(item, dict) and item.get("action") == "awaiting_physical_mouse"
                        and item.get("target") == "wave_count"
                        and isinstance(item.get("acceptance_nonce"), str)
                        and isinstance(session_nonce, str)
                        and item["acceptance_nonce"].lower() == session_nonce.lower()
                        and isinstance(item.get("creator_build_sha256"), str)
                        and isinstance(baseline.get("creator_build_sha256"), str)
                        and item["creator_build_sha256"].lower()
                            == baseline["creator_build_sha256"].lower()
                        for item in actions)
                and not any(
                    isinstance(item, dict) and isinstance(item.get("action"), str)
                    and item["action"] in {
                        "relative_mouse_move_sent", "numeric_slider_motion_plan",
                        "numeric_slider_drag_started",
                    } for item in actions)
            )
            report_valid = (
                isinstance(report, dict)
                and report.get("kind") == "OS_mouse_keyboard_creator_parameters"
                and report.get("passed") is True
                and report.get("manual_slider") is True
                and report.get("numeric_slider_verified") is True
                and report.get("slider_input_mode") == "awaited_physical_mouse"
                and report.get("human_ui_validated") is False
                and report.get("physical_input_provenance_verified") is False
                and report.get("automated_os_input") is True
                and report.get("automated_slider_motion") is False
                and report.get("creator_build_unchanged_during_run") is True
                and isinstance(report.get("window_title"), str)
                and isinstance(session_nonce, str)
                and report["window_title"]
                    == f"AkUGC Physical Slider [{session_nonce.lower()}]"
                and isinstance(report.get("acceptance_nonce"), str)
                and isinstance(session_nonce, str)
                and report["acceptance_nonce"].lower() == session_nonce.lower()
                and isinstance(report.get("creator_build_sha256"), str)
                and isinstance(baseline.get("creator_build_sha256"), str)
                and report["creator_build_sha256"].lower()
                    == baseline["creator_build_sha256"].lower()
                and report.get("checkpoint_count") == 10
                and report.get("expected_checkpoints") == 10
                and observed_slider_evidence(report.get("slider_evidence"))
                and checkpoint_valid
                and actions_valid
            )
            validator.check(
                report_valid,
                "manual slider report must contain a successful awaited-physical-mouse Slate observation "
                "without claiming software-certified HID or human provenance",
            )
    evidence = value.get("evidence_files")
    validator.check(isinstance(evidence, list) and len(evidence) >= 2,
                    "physical_slider.evidence_files must contain before/after or recording artifacts")
    if isinstance(evidence, list):
        validator.check(len(evidence) == len(set(item for item in evidence if isinstance(item, str))),
                        "physical_slider.evidence_files must not reuse an artifact")
        for index, item in enumerate(evidence):
            validator.evidence_file(
                item, f"physical_slider.evidence_files[{index}]", role="slider_artifact")
        validator.unique_evidence_content(evidence, "physical_slider.evidence_files")
    metrics["physical_slider_observed"] = bool(
        report_valid and value.get("used_physical_mouse") is True
        and value.get("automated_slider_motion_disabled") is True
        and value.get("observer_attested") is True
    )
    return metrics


def validate_usability(value, baseline: dict, validator: Validator) -> dict:
    metrics = {"duration_minutes": None}
    if not validator.exact_keys(value, USABILITY_KEYS, "usability"):
        value = value if isinstance(value, dict) else {}
    validator.nonempty(value.get("tester_alias"), "usability.tester_alias")
    validator.nonempty(value.get("observer_alias"), "usability.observer_alias")
    validator.truth(value.get("tester_is_non_programmer"), "usability.tester_is_non_programmer")
    validator.truth(value.get("evidence_capture_consent"), "usability.evidence_capture_consent")
    started = validator.timestamp(value.get("started_at_utc"), "usability.started_at_utc")
    finished = validator.timestamp(value.get("finished_at_utc"), "usability.finished_at_utc")
    if started and finished:
        duration = (finished - started).total_seconds() / 60
        metrics["duration_minutes"] = round(duration, 3)
        validator.check(0 < duration <= 60, "usability session must finish within 60 minutes")
    validator.check(value.get("used_logic_graph") is False,
                    "usability.used_logic_graph must be explicitly false for the L1 first-work gate")
    for field in (
        "created_project", "saved_project", "published_work", "exported_share",
        "recipient_imported_and_played", "independent_key_channel_verified", "observer_attested",
    ):
        validator.truth(value.get(field), f"usability.{field}")
    creator = value.get("creator_device")
    recipient = value.get("recipient_device")
    validator.nonempty(creator, "usability.creator_device")
    validator.nonempty(recipient, "usability.recipient_device")
    validator.check(bool(creator and recipient and creator != recipient),
                    "usability creator and recipient must be distinct physical device labels")
    validator.hash_value(
        value.get("creator_package_sha256"), "usability.creator_package_sha256")
    validator.check(
        isinstance(value.get("creator_package_sha256"), str)
        and isinstance(baseline.get("creator_package_sha256"), str)
        and value["creator_package_sha256"].lower()
            == baseline["creator_package_sha256"].lower(),
        "usability.creator_package_sha256 differs from the frozen Creator package",
    )
    validator.check(isinstance(value.get("notes"), str), "usability.notes must be text")

    share_path = validator.evidence_file(
        value.get("share_file"), "usability.share_file", ".akugc", "share")
    validator.hash_value(value.get("share_sha256"), "usability.share_sha256")
    validator.hash_value(value.get("trusted_public_key"), "usability.trusted_public_key")
    if share_path and HEX_64.fullmatch(str(value.get("share_sha256", ""))):
        validator.check(
            validator.evidence_hashes.get(Path(value["share_file"]).as_posix())
            == value["share_sha256"].lower(),
                        "usability share SHA-256 does not match the evidence file")
    if share_path and HEX_64.fullmatch(str(value.get("trusted_public_key", ""))):
        try:
            package = parse_work_package(
                validator.bytes_for(value.get("share_file")) or b"",
                value["trusted_public_key"])
        except ValueError as error:
            validator.diagnostics.append(f"usability share package failed signed metadata validation: {error}")
        else:
            metrics["first_work_release_id"] = package["release_id"]
            metrics["first_work_content_hash"] = package["content_hash"]
    evidence = value.get("evidence_files")
    validator.check(isinstance(evidence, list) and len(evidence) >= 4,
                    "usability.evidence_files must contain at least four start/create/share/play artifacts")
    if isinstance(evidence, list):
        validator.check(len(evidence) == len(set(item for item in evidence if isinstance(item, str))),
                        "usability.evidence_files must not reuse an artifact")
        for index, item in enumerate(evidence):
            validator.evidence_file(
                item, f"usability.evidence_files[{index}]", role="usability_artifact")
        validator.unique_evidence_content(evidence, "usability.evidence_files")
    return metrics


def expected_package_hash(role: str, baseline: dict):
    return baseline.get({
        "client": "client_package_sha256",
        "game": "game_package_sha256",
        "server": "server_package_sha256",
    }.get(role, ""))


def validate_observed_endpoint(value, expected_keys: set[str], label: str, baseline: dict,
                               release_id: str | None, content_hash: str | None,
                               validator: Validator, screenshot_required: bool):
    if not validator.exact_keys(value, expected_keys, label):
        value = value if isinstance(value, dict) else {}
    validator.nonempty(value.get("device"), label + ".device")
    role = value.get("package_role")
    validator.check(isinstance(role, str) and role in {"client", "game", "server"},
                    label + ".package_role must be client, game, or server")
    validator.hash_value(value.get("package_sha256"), label + ".package_sha256")
    expected_package = expected_package_hash(role, baseline) if isinstance(role, str) else None
    validator.check(
        isinstance(value.get("package_sha256"), str)
        and isinstance(expected_package, str)
        and value["package_sha256"].lower() == expected_package.lower(),
        label + ".package_sha256 differs from the frozen role baseline",
    )
    validator.check(
        isinstance(value.get("release_id"), str) and isinstance(release_id, str)
        and value["release_id"].lower() == release_id.lower(),
        label + ".release_id differs from the signed work package",
    )
    validator.check(
        isinstance(value.get("content_hash"), str) and isinstance(content_hash, str)
        and value["content_hash"].lower() == content_hash.lower(),
        label + ".content_hash differs from the signed work package",
    )
    validator.check(value.get("result") == "Victory", label + ".result must be Victory")
    for field in ("team_score", "base_health", "resources"):
        validator.check(type(value.get(field)) is int and value[field] >= 0,
                        f"{label}.{field} must be a non-negative integer")
    validator.evidence_file(value.get("log"), label + ".log", role="endpoint_log")
    if screenshot_required:
        validator.evidence_file(
            value.get("screenshot"), label + ".screenshot", role="endpoint_screenshot")
    return value


def validate_participant(value, index: int, run_index: int, baseline: dict,
                         release_id: str | None, content_hash: str | None,
                         validator: Validator):
    label = f"cross_device.seed_runs[{run_index}].participants[{index}]"
    return validate_observed_endpoint(
        value, PARTICIPANT_KEYS, label, baseline, release_id, content_hash, validator, True)


def validate_host_observation(value, run_index: int, baseline: dict, release_id: str | None,
                              content_hash: str | None, host_mode: str, host_device: str,
                              validator: Validator):
    label = f"cross_device.seed_runs[{run_index}].host_observation"
    result = validate_observed_endpoint(
        value, HOST_OBSERVATION_KEYS, label, baseline, release_id, content_hash, validator, False)
    expected_role = "game" if host_mode == "listen" else "server"
    validator.check(result.get("package_role") == expected_role,
                    label + f".package_role must be {expected_role} for {host_mode} hosting")
    validator.check(
        isinstance(result.get("device"), str) and result.get("device") == host_device,
        label + ".device must equal the seed host_device",
    )
    return result


def validate_host_identity(value: dict, label: str, baseline: dict, validator: Validator) -> None:
    mode = value.get("host_mode")
    validator.check(
        isinstance(mode, str) and mode in {"listen", "dedicated"},
        label + ".host_mode must be listen or dedicated")
    validator.hash_value(value.get("host_package_sha256"), label + ".host_package_sha256")
    expected = baseline.get("game_package_sha256" if mode == "listen" else "server_package_sha256")
    validator.check(
        isinstance(value.get("host_package_sha256"), str)
        and isinstance(expected, str)
        and value["host_package_sha256"].lower() == expected.lower(),
        label + ".host_package_sha256 differs from the frozen Game/Server baseline",
    )


def validate_seed_runs(values, baseline: dict, validator: Validator) -> dict:
    metrics = {
        "seed_count": 0, "player_counts": [], "host_modes": [],
        "tamper_rejections": 0, "latest_seed_observed_at_utc": None}
    if not isinstance(values, list):
        validator.diagnostics.append("cross_device.seed_runs must be an array")
        return metrics
    seed_ids, release_ids, share_hashes = [], [], []
    signed_content_hashes, payload_fingerprints = [], []
    participant_artifacts = []
    player_counts = set()
    host_modes = set()
    observed_times = []
    for run_index, original in enumerate(values):
        label = f"cross_device.seed_runs[{run_index}]"
        value = original if isinstance(original, dict) else {}
        validator.exact_keys(original, SEED_KEYS, label)
        validator.nonempty(value.get("seed_id"), label + ".seed_id")
        validator.check(isinstance(value.get("release_id"), str)
                        and HEX_32.fullmatch(value["release_id"]) is not None,
                        label + ".release_id must be a 32-digit hexadecimal ID")
        validator.hash_value(value.get("share_sha256"), label + ".share_sha256")
        validator.hash_value(value.get("trusted_public_key"), label + ".trusted_public_key")
        share_path = validator.evidence_file(
            value.get("share_file"), label + ".share_file", ".akugc", "share")
        package = None
        if share_path and HEX_64.fullmatch(str(value.get("share_sha256", ""))):
            validator.check(
                validator.evidence_hashes.get(Path(value["share_file"]).as_posix())
                == value["share_sha256"].lower(),
                            label + " share SHA-256 does not match the evidence file")
        if share_path and HEX_64.fullmatch(str(value.get("trusted_public_key", ""))):
            try:
                package = parse_work_package(
                    validator.bytes_for(value.get("share_file")) or b"",
                    value["trusted_public_key"])
            except ValueError as error:
                validator.diagnostics.append(f"{label} failed signed metadata validation: {error}")
            else:
                validator.check(package["release_id"] == str(value.get("release_id", "")).lower(),
                                label + " release_id differs from the signed package manifest")
                signed_content_hashes.append(package["content_hash"])
                payload_fingerprints.append(package["payload_fingerprint"])
        validator.truth(value.get("transferred_between_devices"), label + ".transferred_between_devices")
        validator.truth(value.get("independent_key_channel_verified"),
                        label + ".independent_key_channel_verified")
        validator.nonempty(value.get("host_device"), label + ".host_device")
        validate_host_identity(value, label, baseline, validator)
        if isinstance(value.get("host_mode"), str) and value["host_mode"] in {
                "listen", "dedicated"}:
            host_modes.add(value["host_mode"])
        validator.nonempty(value.get("observer_alias"), label + ".observer_alias")
        observed_at = validator.timestamp(
            value.get("observed_at_utc"), label + ".observed_at_utc")
        if observed_at:
            observed_times.append(observed_at)
        validator.truth(value.get("observer_attested"), label + ".observer_attested")
        validator.check(isinstance(value.get("notes"), str), label + ".notes must be text")
        if value.get("tampered_copy_rejected") is True:
            metrics["tamper_rejections"] += 1
            tampered_path = validator.evidence_file(
                value.get("tampered_copy_file"), label + ".tampered_copy_file",
                ".akugc", "tampered_share")
            validator.hash_value(
                value.get("tampered_copy_sha256"), label + ".tampered_copy_sha256")
            rejection_path = validator.evidence_file(
                value.get("tamper_rejection_evidence"), label + ".tamper_rejection_evidence",
                ".json", "tamper_rejection")
            if tampered_path and share_path:
                validator.check(
                    validator.bytes_for(value.get("tampered_copy_file"))
                    != validator.bytes_for(value.get("share_file")),
                    label + ".tampered_copy_file must differ from the accepted share",
                )
                tampered_digest = validator.evidence_hashes.get(
                    Path(value["tampered_copy_file"]).as_posix())
                validator.check(
                    isinstance(value.get("tampered_copy_sha256"), str)
                    and tampered_digest == value["tampered_copy_sha256"].lower(),
                    label + ".tampered_copy_sha256 does not match tampered_copy_file",
                )
                try:
                    tampered_package = parse_work_package(
                        validator.bytes_for(value.get("tampered_copy_file")) or b"",
                        value.get("trusted_public_key"))
                except ValueError as error:
                    validator.diagnostics.append(
                        f"{label} tampered copy must preserve signed envelope identity: {error}")
                else:
                    validator.check(
                        package is not None
                        and tampered_package["release_id"] == package["release_id"]
                        and tampered_package["content_hash"] == package["content_hash"]
                        and tampered_package["manifest_fingerprint"]
                            == package["manifest_fingerprint"]
                        and tampered_package["signature_fingerprint"]
                            == package["signature_fingerprint"],
                        label + " tampered copy must preserve the exact signed envelope",
                    )
                    validator.check(
                        package is not None
                        and tampered_package["payload_fingerprint"]
                            != package["payload_fingerprint"],
                        label + " tampered copy must change the document/program payload",
                    )
            if rejection_path:
                try:
                    rejection = strict_json_loads(
                        (validator.bytes_for(value.get("tamper_rejection_evidence")) or b"")
                        .decode("utf-8-sig"))
                except (UnicodeError, json.JSONDecodeError, ValueError, RecursionError):
                    rejection = None
                valid_rejection = (
                    isinstance(rejection, dict)
                    and set(rejection) == {
                        "tampered_share_sha256", "release_id", "content_hash",
                        "rejected_by_product_loader"}
                    and isinstance(rejection.get("tampered_share_sha256"), str)
                    and isinstance(rejection.get("release_id"), str)
                    and isinstance(rejection.get("content_hash"), str)
                    and isinstance(value.get("tampered_copy_sha256"), str)
                    and rejection["tampered_share_sha256"].lower()
                        == value["tampered_copy_sha256"].lower()
                    and package is not None
                    and rejection["release_id"].lower() == package["release_id"]
                    and rejection["content_hash"].lower() == package["content_hash"]
                    and rejection.get("rejected_by_product_loader") is True
                )
                validator.check(
                    valid_rejection,
                    label + ".tamper_rejection_evidence must bind product rejection "
                    "to the tampered copy",
                )
        else:
            validator.check(value.get("tampered_copy_rejected") is False,
                            label + ".tampered_copy_rejected must be a boolean")
            validator.check(value.get("tampered_copy_file") == "",
                            label + ".tampered_copy_file must be empty when no rejection was observed")
            validator.check(value.get("tampered_copy_sha256") == "",
                            label + ".tampered_copy_sha256 must be empty when no rejection was observed")
            validator.check(value.get("tamper_rejection_evidence") == "",
                            label + ".tamper_rejection_evidence must be empty when no rejection was observed")

        participants = value.get("participants")
        validator.check(isinstance(participants, list) and 2 <= len(participants) <= 4,
                        label + ".participants must contain 2 to 4 devices")
        validated = []
        package_release_id = package["release_id"] if package else None
        package_content_hash = package["content_hash"] if package else None
        if isinstance(participants, list):
            validated = [
                validate_participant(
                    participant, index, run_index, baseline, package_release_id,
                    package_content_hash, validator)
                for index, participant in enumerate(participants)
            ]
        if value.get("host_mode") == "listen":
            host_reference = value.get("host_observation")
            validator.exact_keys(
                host_reference, LISTEN_HOST_OBSERVATION_KEYS,
                f"cross_device.seed_runs[{run_index}].host_observation")
            participant_index = (
                host_reference.get("participant_index")
                if isinstance(host_reference, dict) else None)
            validator.check(
                type(participant_index) is int and 0 <= participant_index < len(validated),
                f"cross_device.seed_runs[{run_index}].host_observation.participant_index "
                "must identify a player",
            )
            host = (
                validated[participant_index]
                if type(participant_index) is int and 0 <= participant_index < len(validated)
                else {})
            validator.check(
                host.get("package_role") == "game"
                and host.get("device") == value.get("host_device"),
                f"cross_device.seed_runs[{run_index}].host_observation must reference "
                "the Listen Game participant",
            )
        else:
            host = validate_host_observation(
                value.get("host_observation"), run_index, baseline, package_release_id,
                package_content_hash, value.get("host_mode"), value.get("host_device"), validator)
        devices = [item.get("device") for item in validated if isinstance(item.get("device"), str)]
        participant_artifacts.extend(
            item.get(field) for item in validated for field in ("log", "screenshot")
            if isinstance(item.get(field), str)
        )
        validator.check(len(devices) == len(set(devices)), label + " participant device labels must be unique")
        game_participants = [
            item for item in validated if item.get("package_role") == "game"]
        if value.get("host_mode") == "listen":
            validator.check(
                len(game_participants) == 1
                and game_participants[0].get("device") == value.get("host_device"),
                label + " listen run must include exactly one Game participant matching host_device",
            )
            validator.check(all(
                isinstance(item.get("package_role"), str)
                and item["package_role"] in {"client", "game"} for item in validated),
                label + " listen participants must use only Game/Client packages",
            )
        elif value.get("host_mode") == "dedicated":
            validator.check(
                all(item.get("package_role") == "client" for item in validated),
                label + " dedicated participants must all use the Client package",
            )
            validator.check(
                value.get("host_device") not in devices,
                label + " dedicated host_device must not be counted as a player participant",
            )
        if 2 <= len(validated) <= 4:
            player_counts.add(len(validated))
        for field in ("result", "team_score", "base_health", "resources"):
            observed = [item.get(field) for item in validated] + [host.get(field)]
            validator.check(bool(observed) and all(item == observed[0] for item in observed),
                            f"{label} player/authority observations disagree on {field}")
        seed_ids.append(value.get("seed_id"))
        release_ids.append(
            value["release_id"].lower()
            if isinstance(value.get("release_id"), str) else value.get("release_id"))
        share_hashes.append(str(value.get("share_sha256", "")).lower())

    validator.check(len(values) >= 10, "cross-device gate requires at least 10 seed runs")
    validator.check(all(isinstance(item, str) for item in seed_ids)
                    and len(seed_ids) == len(set(seed_ids)), "seed IDs must be unique")
    validator.check(all(isinstance(item, str) for item in release_ids)
                    and len(release_ids) == len(set(release_ids)), "release IDs must be unique")
    validator.check(len(share_hashes) == len(set(share_hashes)), "seed share hashes must be unique")
    validator.check(len(signed_content_hashes) == len(values)
                    and len(signed_content_hashes) == len(set(signed_content_hashes)),
                    "seed signed content hashes must be valid and unique")
    validator.check(len(payload_fingerprints) == len(values)
                    and len(payload_fingerprints) == len(set(payload_fingerprints)),
                    "seed document/program payloads must be valid and unique")
    validator.check(len(participant_artifacts) == len(set(participant_artifacts)),
                    "cross-device participant logs/screenshots must not be reused between observations")
    validator.unique_evidence_content(
        participant_artifacts, "cross-device participant logs/screenshots")
    validator.check({2, 4}.issubset(player_counts),
                    "cross-device seed runs must include both 2-player and 4-player physical-device coverage")
    validator.check(host_modes == {"listen", "dedicated"},
                    "cross-device seed runs must include both Listen and Dedicated hosts")
    validator.check(metrics["tamper_rejections"] >= 1,
                    "at least one transferred seed must demonstrate tampered-copy rejection")
    metrics["seed_count"] = len(values)
    metrics["player_counts"] = sorted(player_counts)
    metrics["host_modes"] = sorted(host_modes)
    if observed_times:
        metrics["latest_seed_observed_at_utc"] = max(observed_times).isoformat()
    return metrics


def validate_resilience_runs(values, baseline: dict, validator: Validator) -> dict:
    metrics = {"resilience_cases": [], "latest_resilience_observed_at_utc": None}
    if not isinstance(values, list):
        validator.diagnostics.append("cross_device.resilience_runs must be an array")
        return metrics
    cases = set()
    evidence_logs = []
    observed_times = []
    for index, original in enumerate(values):
        label = f"cross_device.resilience_runs[{index}]"
        value = original if isinstance(original, dict) else {}
        validator.exact_keys(original, RESILIENCE_KEYS, label)
        case = value.get("case")
        validator.check(isinstance(case, str) and case in RESILIENCE_CASES,
                        label + ".case must be client_leave, network_loss, host_terminated, or reconnect")
        if isinstance(case, str) and case in RESILIENCE_CASES:
            cases.add(case)
        validate_host_identity(value, label, baseline, validator)
        validator.nonempty(value.get("host_device"), label + ".host_device")
        validator.nonempty(value.get("observer_alias"), label + ".observer_alias")
        observed_at = validator.timestamp(
            value.get("observed_at_utc"), label + ".observed_at_utc")
        if observed_at:
            observed_times.append(observed_at)
        if case == "host_terminated":
            validator.check(value.get("host_mode") == "listen",
                            label + ".host_terminated must exercise a Listen host")
        for field in (
            "fault_action_observed", "net_driver_cleared", "roster_cleared", "session_cleared",
            "recovered_to_clean_state", "followup_play_succeeded", "observer_attested",
        ):
            validator.truth(value.get(field), f"{label}.{field}")
        validator.check(type(value.get("new_process_used")) is bool,
                        label + ".new_process_used must be a boolean")
        if isinstance(case, str) and case in {"host_terminated", "reconnect"}:
            validator.truth(value.get("new_process_used"), label + ".new_process_used")
        reports = value.get("device_reports")
        validator.check(isinstance(reports, list) and len(reports) >= 2,
                        label + ".device_reports must contain evidence from at least two devices")
        validated_reports = []
        if isinstance(reports, list):
            for report_index, original_report in enumerate(reports):
                report_label = f"{label}.device_reports[{report_index}]"
                report = original_report if isinstance(original_report, dict) else {}
                validator.exact_keys(
                    original_report, RESILIENCE_DEVICE_REPORT_KEYS, report_label)
                validator.nonempty(report.get("device"), report_label + ".device")
                role = report.get("package_role")
                validator.check(
                    isinstance(role, str) and role in {"client", "game", "server"},
                                report_label + ".package_role must be client, game, or server")
                validator.hash_value(report.get("package_sha256"),
                                     report_label + ".package_sha256")
                expected = expected_package_hash(role, baseline) if isinstance(role, str) else None
                validator.check(
                    isinstance(report.get("package_sha256"), str)
                    and isinstance(expected, str)
                    and report["package_sha256"].lower() == expected.lower(),
                    report_label + ".package_sha256 differs from the frozen role baseline",
                )
                validator.nonempty(
                    report.get("before_process_instance"),
                    report_label + ".before_process_instance")
                validator.nonempty(
                    report.get("after_process_instance"),
                    report_label + ".after_process_instance")
                if isinstance(report.get("log"), str):
                    evidence_logs.append(report["log"])
                validator.evidence_file(
                    report.get("log"), report_label + ".log", role="resilience_log")
                validated_reports.append(report)
        devices = [
            report.get("device") for report in validated_reports
            if isinstance(report.get("device"), str)]
        validator.check(len(devices) == len(set(devices)),
                        label + ".device_reports must use unique device labels")
        expected_host_role = "game" if value.get("host_mode") == "listen" else "server"
        host_reports = [
            report for report in validated_reports
            if report.get("device") == value.get("host_device")]
        validator.check(
            len(host_reports) == 1 and host_reports[0].get("package_role") == expected_host_role,
            label + f" must contain one {expected_host_role} report for host_device",
        )
        validator.check(any(
            report.get("package_role") == "client" for report in validated_reports),
            label + " must contain at least one Client device report",
        )
        fault_device = value.get("fault_device")
        recovery_device = value.get("recovery_device")
        validator.nonempty(fault_device, label + ".fault_device")
        validator.nonempty(recovery_device, label + ".recovery_device")
        fault_reports = [
            report for report in validated_reports if report.get("device") == fault_device]
        validator.check(len(fault_reports) == 1,
                        label + ".fault_device must identify one device report")
        recovery_reports = [
            report for report in validated_reports
            if report.get("device") == recovery_device]
        validator.check(len(recovery_reports) == 1,
                        label + ".recovery_device must identify one device report")
        validator.check(
            isinstance(fault_device, str) and recovery_device == fault_device,
            label + ".recovery_device must be the same device that experienced the fault",
        )
        if case == "host_terminated":
            validator.check(
                fault_device == value.get("host_device")
                and bool(fault_reports) and fault_reports[0].get("package_role") == "game",
                label + ".host_terminated fault must target the Listen Game host",
            )
        elif isinstance(case, str) and case in {"client_leave", "network_loss", "reconnect"}:
            validator.check(
                bool(fault_reports) and fault_reports[0].get("package_role") == "client",
                label + f".{case} fault must target a Client device",
            )
        recovery_report = recovery_reports[0] if len(recovery_reports) == 1 else {}
        before_instance = recovery_report.get("before_process_instance")
        after_instance = recovery_report.get("after_process_instance")
        if value.get("new_process_used") is True:
            validator.check(
                isinstance(before_instance, str) and isinstance(after_instance, str)
                and before_instance != after_instance,
                label + " recovery-device report requires distinct before/after process instances",
            )
        validator.evidence_file(
            value.get("fault_evidence"), label + ".fault_evidence", role="fault_action")
        validator.evidence_file(
            value.get("recovery_evidence"), label + ".recovery_evidence", role="recovery_action")
        validator.check(isinstance(value.get("notes"), str), label + ".notes must be text")
    validator.check(len(evidence_logs) == len(set(evidence_logs)),
                    "cross-device resilience logs must not be reused between cases/devices")
    validator.unique_evidence_content(evidence_logs, "cross-device resilience logs")
    missing = sorted(RESILIENCE_CASES - cases)
    validator.check(not missing, "missing cross-device resilience cases: " + ", ".join(missing))
    metrics["resilience_cases"] = sorted(cases)
    if observed_times:
        metrics["latest_resilience_observed_at_utc"] = max(observed_times).isoformat()
    return metrics


def validate_manifest(manifest, evidence_root: Path) -> dict:
    validator = Validator(evidence_root)
    if not validator.exact_keys(manifest, TOP_KEYS, "manifest"):
        manifest = manifest if isinstance(manifest, dict) else {}
    validator.check(manifest.get("schema_version") == SCHEMA_VERSION,
                    f"schema_version must be {SCHEMA_VERSION}")
    validator.hash_value(manifest.get("session_nonce"), "session_nonce", HEX_32)
    lock_name = manifest.get("evidence_lock")
    validator.check(isinstance(lock_name, str) and bool(lock_name)
                    and not Path(lock_name).is_absolute() and ".." not in Path(lock_name).parts,
                    "evidence_lock must be a relative in-directory path")

    baseline = manifest.get("baseline")
    if not validator.exact_keys(baseline, BASELINE_KEYS, "baseline"):
        baseline = baseline if isinstance(baseline, dict) else {}
    validator.hash_value(baseline.get("source_commit"), "baseline.source_commit", HEX_40)
    baseline_files = []
    baseline_paths = {}
    for role in (
            "creator_build", "creator_package", "game_package", "client_package", "server_package"):
        file_field, hash_field = role + "_file", role + "_sha256"
        validator.hash_value(baseline.get(hash_field), f"baseline.{hash_field}")
        path = validator.evidence_file(
            baseline.get(file_field), f"baseline.{file_field}",
            ".dll" if role == "creator_build" else ".zip",
            role="baseline_" + role)
        baseline_paths[role] = path
        if isinstance(baseline.get(file_field), str):
            baseline_files.append(baseline[file_field])
        if path and HEX_64.fullmatch(str(baseline.get(hash_field, ""))):
            validator.check(
                validator.evidence_hashes.get(Path(baseline[file_field]).as_posix())
                == baseline[hash_field].lower(),
                            f"baseline.{hash_field} does not match baseline.{file_field}")
    validator.check(len(baseline_files) == len(set(baseline_files)),
                    "baseline build/package files must be distinct")
    validator.unique_evidence_content(baseline_files, "baseline build/package files")
    if baseline_paths.get("creator_package"):
        try:
            creator_package = inspect_creator_package(baseline_paths["creator_package"])
        except ValueError as error:
            validator.diagnostics.append(str(error))
        else:
            validator.check(
                isinstance(baseline.get("creator_package_sha256"), str)
                and creator_package["archive_sha256"]
                    == baseline["creator_package_sha256"].lower(),
                "Creator package inspection snapshot differs from its frozen SHA-256",
            )
            validator.check(
                isinstance(baseline.get("creator_build_sha256"), str)
                and creator_package["creator_module_sha256"]
                    == baseline["creator_build_sha256"].lower(),
                "Creator package AkUGCEditor module differs from creator_build baseline",
            )
    for role in ("game_package", "client_package", "server_package"):
        if baseline_paths.get(role):
            try:
                runtime_package = inspect_runtime_package(baseline_paths[role], role)
            except ValueError as error:
                validator.diagnostics.append(str(error))
            else:
                validator.check(
                    isinstance(baseline.get(role + "_sha256"), str)
                    and runtime_package["archive_sha256"]
                        == baseline[role + "_sha256"].lower(),
                    f"{role} inspection snapshot differs from its frozen SHA-256",
                )

    slider_metrics = validate_physical_slider(
        manifest.get("physical_slider"), manifest.get("session_nonce"), baseline, validator)
    usability_metrics = validate_usability(manifest.get("usability"), baseline, validator)
    cross = manifest.get("cross_device")
    if not validator.exact_keys(cross, CROSS_DEVICE_KEYS, "cross_device"):
        cross = cross if isinstance(cross, dict) else {}
    seed_metrics = validate_seed_runs(cross.get("seed_runs"), baseline, validator)
    resilience_metrics = validate_resilience_runs(cross.get("resilience_runs"), baseline, validator)

    attestation = manifest.get("final_attestation")
    if not validator.exact_keys(attestation, ATTESTATION_KEYS, "final_attestation"):
        attestation = attestation if isinstance(attestation, dict) else {}
    validator.nonempty(attestation.get("coordinator_alias"), "final_attestation.coordinator_alias")
    for field in (
        "devices_are_physical_and_distinct", "real_network_used", "real_file_transfer_used",
        "same_machine_processes_not_counted", "claims_are_human_attested_not_software_proven",
        "evidence_unmodified", "signed_off",
    ):
        validator.truth(attestation.get(field), f"final_attestation.{field}")
    signed_at = validator.timestamp(
        attestation.get("signed_at_utc"), "final_attestation.signed_at_utc")
    if signed_at:
        validator.check(
            signed_at <= datetime.now(timezone.utc),
            "final_attestation.signed_at_utc must not be in the future")
    finished_at = None
    usability_for_time = manifest.get("usability")
    if isinstance(usability_for_time, dict):
        try:
            finished_at = datetime.fromisoformat(
                str(usability_for_time.get("finished_at_utc", "")).replace("Z", "+00:00"))
        except ValueError:
            pass
    completed_times = [
        item for item in (finished_at,) if item is not None and item.tzinfo is not None]
    for metric_value in (
            slider_metrics.get("physical_slider_observed_at_utc"),
            seed_metrics.get("latest_seed_observed_at_utc"),
            resilience_metrics.get("latest_resilience_observed_at_utc")):
        if isinstance(metric_value, str):
            try:
                completed_times.append(datetime.fromisoformat(metric_value))
            except ValueError:
                pass
    if signed_at and completed_times:
        validator.check(
            signed_at >= max(completed_times),
            "final attestation must be signed after all recorded acceptance observations")
    validator.hash_value(
        attestation.get("attestation_sha256"), "final_attestation.attestation_sha256")
    attestation_path = validator.evidence_file(
        attestation.get("attestation_file"), "final_attestation.attestation_file",
        role="human_attestation")
    if attestation_path and HEX_64.fullmatch(str(attestation.get("attestation_sha256", ""))):
        validator.check(
            validator.evidence_hashes.get(Path(attestation["attestation_file"]).as_posix())
            == attestation["attestation_sha256"].lower(),
            "final_attestation SHA-256 does not match attestation_file",
        )
    slider_roles = manifest.get("physical_slider")
    slider_roles = slider_roles if isinstance(slider_roles, dict) else {}
    usability_roles = manifest.get("usability")
    usability_roles = usability_roles if isinstance(usability_roles, dict) else {}
    tester_aliases = {
        value for value in (
            slider_roles.get("tester_alias"),
            usability_roles.get("tester_alias"),
        ) if isinstance(value, str) and value
    }
    observer_aliases = {
        value for value in (
            slider_roles.get("observer_alias"),
            usability_roles.get("observer_alias"),
        ) if isinstance(value, str) and value
    }
    cross_roles = manifest.get("cross_device")
    cross_roles = cross_roles if isinstance(cross_roles, dict) else {}
    for collection in ("seed_runs", "resilience_runs"):
        entries = cross_roles.get(collection)
        if isinstance(entries, list):
            observer_aliases.update(
                entry.get("observer_alias") for entry in entries
                if isinstance(entry, dict)
                and isinstance(entry.get("observer_alias"), str)
                and entry.get("observer_alias"))
    coordinator_alias = attestation.get("coordinator_alias")
    validator.check(not tester_aliases.intersection(observer_aliases),
                    "tester and observer aliases must represent separate roles")
    validator.check(
        not isinstance(coordinator_alias, str)
        or coordinator_alias not in tester_aliases.union(observer_aliases),
        "coordinator alias must be separate from tester and observer roles",
    )
    validator.verify_unchanged()

    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "phase1_external_acceptance_evidence_validation",
        "passed": not validator.diagnostics,
        "human_attested": bool(attestation.get("signed_off")),
        "software_independently_verified_human_claims": False,
        "diagnostics": validator.diagnostics,
        "metrics": {
            **slider_metrics,
            **usability_metrics,
            **seed_metrics,
            **resilience_metrics,
            "evidence_file_count": len(validator.evidence_hashes),
        },
        "evidence_sha256": dict(sorted(validator.evidence_hashes.items())),
        "boundary": (
            "The validator checks files, hashes, timing, signed manifest metadata, payload uniqueness, "
            "archive/PE/receipt/Pak structure, observer-entered cross-device value consistency, and "
            "explicit attestations. It does not recompute the product's full ContentHash or independently "
            "prove executable runtime semantics, receipt-to-binary build provenance, tester background, "
            "physical HID origin, distinct hardware, real network conditions, consent, or truthfulness "
            "of human observations."
        ),
    }


def validate_evidence_lock(manifest: dict, raw_manifest: bytes, result: dict, root: Path) -> None:
    lock_name = manifest.get("evidence_lock")
    if not isinstance(lock_name, str) or not lock_name:
        result["diagnostics"].append("evidence lock path is missing")
        result["passed"] = False
        result["metrics"]["evidence_lock_valid"] = False
        return
    lock_path = root / lock_name
    try:
        if (lock_path.is_symlink() or not lock_path.resolve().is_relative_to(root.resolve())
                or lock_path.stat().st_size > 1024 * 1024):
            raise ValueError("unsafe evidence lock")
        lock = strict_json_loads(lock_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError, RecursionError):
        result["diagnostics"].append("evidence lock is missing or invalid")
        result["passed"] = False
        result["metrics"]["evidence_lock_valid"] = False
        return
    expected_keys = {
        "schema_version", "kind", "session_nonce", "sealed_at_utc", "manifest_sha256",
        "evidence_sha256",
    }
    valid = (
        isinstance(lock, dict)
        and set(lock) == expected_keys
        and lock.get("schema_version") == SCHEMA_VERSION
        and lock.get("kind") == "phase1_external_acceptance_evidence_lock"
        and lock.get("session_nonce") == manifest.get("session_nonce")
        and lock.get("manifest_sha256") == hashlib.sha256(raw_manifest).hexdigest()
        and lock.get("evidence_sha256") == result.get("evidence_sha256")
    )
    try:
        sealed_value = lock.get("sealed_at_utc", "") if isinstance(lock, dict) else ""
        sealed = datetime.fromisoformat(str(sealed_value).replace("Z", "+00:00"))
        valid = valid and sealed.tzinfo is not None and sealed.utcoffset() is not None
        attestation = manifest.get("final_attestation")
        signed_value = (
            attestation.get("signed_at_utc", "") if isinstance(attestation, dict) else "")
        signed = datetime.fromisoformat(str(signed_value).replace("Z", "+00:00"))
        valid = (
            valid and signed.tzinfo is not None and signed.utcoffset() is not None
            and sealed >= signed)
    except ValueError:
        valid = False
    if not valid:
        result["diagnostics"].append("evidence lock does not match the sealed manifest/evidence")
        result["passed"] = False
    result["metrics"]["evidence_lock_valid"] = bool(valid)


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    initialize = commands.add_parser("init")
    initialize.add_argument("--output", required=True, type=Path)
    initialize.add_argument("--source-commit", required=True)
    initialize.add_argument("--creator-build", required=True, type=Path)
    initialize.add_argument("--creator-package", required=True, type=Path)
    initialize.add_argument("--game-package", required=True, type=Path)
    initialize.add_argument("--client-package", required=True, type=Path)
    initialize.add_argument("--server-package", required=True, type=Path)
    seal = commands.add_parser("seal")
    seal.add_argument("--input", required=True, type=Path)
    validate = commands.add_parser("validate")
    validate.add_argument("--input", required=True, type=Path)
    validate.add_argument("--output", type=Path)
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_arguments(argv)
    if args.command == "init":
        if args.output.exists():
            raise RuntimeError(f"Refusing to overwrite existing acceptance manifest: {args.output}")
        if args.source_commit and HEX_40.fullmatch(args.source_commit) is None:
            raise RuntimeError("--source-commit must be a 40-digit hexadecimal commit")
        project_root = Path(__file__).resolve().parents[2]
        actual_commit, source_clean = capture_source_state(project_root)
        if args.source_commit.lower() != actual_commit or not source_clean:
            raise RuntimeError(
                "--source-commit must equal HEAD and the source worktree must contain "
                "no tracked changes or non-ignored untracked files")
        artifacts = (
            args.creator_build, args.creator_package, args.game_package,
            args.client_package, args.server_package)
        template = make_template(args.source_commit)
        evidence_root = args.output.resolve().parent
        resolved_artifacts = []
        for label, path in zip(
                ("creator build", "creator package", "game package", "client package",
                 "server package"), artifacts):
            resolved = path.resolve()
            if not resolved.is_relative_to(evidence_root) or path.is_symlink():
                raise RuntimeError(f"{label} must be a regular file inside the acceptance directory: {path}")
            if (not resolved.is_file() or resolved.stat().st_size <= 0
                    or resolved.suffix.lower() == ".key"):
                raise RuntimeError(f"{label} must be a non-empty non-key file: {path}")
            resolved_artifacts.append(resolved)
        if resolved_artifacts[0].suffix.lower() != ".dll":
            raise RuntimeError("--creator-build must be the AkUGCEditor DLL")
        if any(path.suffix.lower() != ".zip" for path in resolved_artifacts[1:]):
            raise RuntimeError("Creator/Game/Client/Server packages must be .zip archives")
        try:
            creator_build_digest = inspect_pe_file(
                resolved_artifacts[0], True, "Creator AkUGCEditor baseline")
            creator_package = inspect_creator_package(resolved_artifacts[1])
            runtime_packages = {}
            for role, package_path in zip(
                    ("game_package", "client_package", "server_package"),
                    resolved_artifacts[2:]):
                runtime_packages[role] = inspect_runtime_package(package_path, role)
        except ValueError as error:
            raise RuntimeError(str(error)) from error
        if creator_package["creator_module_sha256"] != creator_build_digest:
            raise RuntimeError("Creator package module does not match --creator-build")
        frozen_digests = [
            creator_build_digest,
            creator_package["archive_sha256"],
            runtime_packages["game_package"]["archive_sha256"],
            runtime_packages["client_package"]["archive_sha256"],
            runtime_packages["server_package"]["archive_sha256"],
        ]
        if any(
                sha256_file(path) != digest
                for path, digest in zip(resolved_artifacts, frozen_digests)):
            raise RuntimeError("A candidate artifact changed while the baseline was initialized")
        template["baseline"].update({
            "creator_build_file": resolved_artifacts[0].relative_to(evidence_root).as_posix(),
            "creator_build_sha256": creator_build_digest,
            "creator_package_file": resolved_artifacts[1].relative_to(evidence_root).as_posix(),
            "creator_package_sha256": creator_package["archive_sha256"],
            "game_package_file": resolved_artifacts[2].relative_to(evidence_root).as_posix(),
            "game_package_sha256": runtime_packages["game_package"]["archive_sha256"],
            "client_package_file": resolved_artifacts[3].relative_to(evidence_root).as_posix(),
            "client_package_sha256": runtime_packages["client_package"]["archive_sha256"],
            "server_package_file": resolved_artifacts[4].relative_to(evidence_root).as_posix(),
            "server_package_sha256": runtime_packages["server_package"]["archive_sha256"],
        })
        write_json(args.output, template)
        print(args.output.resolve())
        return 0

    input_path = args.input.resolve()
    raw = b""
    manifest = None
    try:
        raw = input_path.read_bytes()
        if len(raw) > 4 * 1024 * 1024:
            raise ValueError("acceptance manifest exceeds 4 MiB")
        manifest = strict_json_loads(raw.decode("utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError, RecursionError) as error:
        result = {
            "schema_version": SCHEMA_VERSION,
            "kind": "phase1_external_acceptance_evidence_validation",
            "passed": False,
            "human_attested": False,
            "software_independently_verified_human_claims": False,
            "diagnostics": [f"Cannot read acceptance manifest: {type(error).__name__}"],
            "metrics": {"evidence_file_count": 0},
            "evidence_sha256": {},
            "boundary": "No human or physical-device claim was validated.",
        }
    else:
        result = validate_manifest(manifest, input_path.parent)
        result["input_sha256"] = hashlib.sha256(raw).hexdigest()

    if args.command == "seal":
        if not result["passed"]:
            print("Cannot seal incomplete acceptance evidence:")
            for diagnostic in result["diagnostics"]:
                print("-", diagnostic)
            return 1
        project_root = Path(__file__).resolve().parents[2]
        actual_commit, source_clean = capture_source_state(project_root)
        baseline = manifest.get("baseline") if isinstance(manifest, dict) else None
        declared_commit = (
            baseline.get("source_commit") if isinstance(baseline, dict) else None)
        if (not isinstance(declared_commit, str)
                or declared_commit.lower() != actual_commit or not source_clean):
            print(
                "Cannot seal unless baseline.source_commit equals the current clean HEAD "
                "and no non-ignored untracked source files exist")
            return 1
        try:
            manifest_unchanged = input_path.read_bytes() == raw
        except OSError:
            manifest_unchanged = False
        fresh_result = validate_manifest(manifest, input_path.parent)
        if (not manifest_unchanged or not fresh_result["passed"]
                or fresh_result["evidence_sha256"] != result["evidence_sha256"]):
            print("Cannot seal evidence that changed during validation")
            return 1
        result = fresh_result
        lock_path = (input_path.parent / manifest["evidence_lock"]).resolve()
        if (not lock_path.is_relative_to(input_path.parent) or lock_path.exists()
                or lock_path == input_path):
            raise RuntimeError("Evidence lock must be a fresh file inside the acceptance directory")
        lock = {
            "schema_version": SCHEMA_VERSION,
            "kind": "phase1_external_acceptance_evidence_lock",
            "session_nonce": manifest["session_nonce"],
            "sealed_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "manifest_sha256": hashlib.sha256(raw).hexdigest(),
            "evidence_sha256": result["evidence_sha256"],
        }
        write_json(lock_path, lock)
        print(lock_path)
        print("external_acceptance_sealed= True")
        return 0

    output_path = (args.output or input_path.with_name("result.json")).resolve()
    if output_path == input_path:
        raise RuntimeError("Validation output must not overwrite the acceptance manifest")
    if output_path.exists():
        raise RuntimeError("Validation output must be a fresh file")
    if output_path.is_relative_to(input_path.parent):
        relative_output = output_path.relative_to(input_path.parent).as_posix()
        protected = set(result.get("evidence_sha256", {}))
        if isinstance(manifest, dict):
            if isinstance(manifest.get("evidence_lock"), str):
                protected.add(Path(manifest["evidence_lock"]).as_posix())
            for value in nested_strings(manifest):
                try:
                    candidate = Path(value)
                    if value and not candidate.is_absolute():
                        protected.add(candidate.as_posix())
                except (OSError, ValueError):
                    pass
        if relative_output in protected:
            raise RuntimeError("Validation output must not overwrite evidence or the evidence lock")
    if isinstance(manifest, dict):
        validate_evidence_lock(manifest, raw, result, input_path.parent)
    write_json(output_path, result)
    print(output_path)
    print("external_acceptance_passed=", result["passed"])
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
