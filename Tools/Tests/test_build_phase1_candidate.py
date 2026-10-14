import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import build_phase1_candidate as build
import phase1_external_acceptance as acceptance


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def write_minimal_pe(path: Path, is_dll: bool) -> None:
    pe_offset = 0x80
    optional_size = 0xF0
    section_raw_offset = 0x200
    section_raw_size = 0x200
    data = bytearray(4096)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe_offset)
    data[pe_offset:pe_offset + 4] = b"PE\0\0"
    characteristics = 0x2022 if is_dll else 0x0022
    struct.pack_into("<HHIIIHH", data, pe_offset + 4,
                     0x8664, 1, 0, 0, 0, optional_size, characteristics)
    optional_offset = pe_offset + 24
    struct.pack_into("<H", data, optional_offset, 0x20B)
    struct.pack_into("<I", data, optional_offset + 16, 0x1000)
    struct.pack_into("<Q", data, optional_offset + 24, 0x140000000)
    struct.pack_into("<I", data, optional_offset + 32, 0x1000)
    struct.pack_into("<I", data, optional_offset + 36, 0x200)
    struct.pack_into("<I", data, optional_offset + 56, 0x2000)
    struct.pack_into("<I", data, optional_offset + 60, 0x200)
    struct.pack_into("<H", data, optional_offset + 68, 2)
    struct.pack_into("<I", data, optional_offset + 108, 16)
    section_offset = optional_offset + optional_size
    data[section_offset:section_offset + 8] = b".text\0\0\0"
    struct.pack_into("<IIIIIIHHI", data, section_offset + 8,
                     0x10, 0x1000, section_raw_size, section_raw_offset,
                     0, 0, 0, 0, 0x60000020)
    data[section_raw_offset] = 0xC3
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_minimal_pak(path: Path) -> None:
    index = b"synthetic-index" * 20
    body = index + b"P" * (4096 - len(index))
    footer = (
        b"\0" * 17
        + acceptance.PAK_MAGIC
        + acceptance.PAK_VERSION_LATEST.to_bytes(4, "little", signed=True)
        + (0).to_bytes(8, "little", signed=True)
        + len(index).to_bytes(8, "little", signed=True)
        + hashlib.sha1(index).digest()
        + b"\0" * acceptance.PAK_COMPRESSION_METHOD_BYTES
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body + footer)


def write_target(path: Path, target: str, target_type: str) -> None:
    executable = f"{target}.exe"
    executable_path = f"$(ProjectDir)/Binaries/Win64/{executable}"
    write_json(path, {
        "TargetName": target,
        "Platform": "Win64",
        "Configuration": "Development",
        "TargetType": target_type,
        "IsTestTarget": False,
        "Architecture": "x64",
        "Launch": executable_path,
        "Version": {
            "BuildId": "12345678-1234-5678-1234-123456789abc",
        },
        "BuildProducts": [{
            "Path": executable_path,
            "Type": "Executable",
        }],
    })


class FreshPhase1BuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.project = self.root / "project"
        write_json(
            self.project / "AkUGCPlatform.uproject",
            {"FileVersion": 3})
        config = self.project / "Config" / "DefaultGame.ini"
        config.parent.mkdir()
        config.write_text(
            "[/Script/UnrealEd.ProjectPackagingSettings]\n"
            '+DirectoriesToAlwaysStageAsUFS=(Path="Lua")\n',
            encoding="utf-8",
        )
        (self.project / "Lua").mkdir()
        (self.project / "Lua" / "Main.lua").write_text(
            "return {}\n", encoding="utf-8")

        self.engine = self.root / "engine"
        write_json(
            self.engine / "Engine/Build/Build.version",
            {
                "MajorVersion": 5,
                "MinorVersion": 8,
                "PatchVersion": 1,
            },
        )
        for relative in (
                "Engine/Build/BatchFiles/Build.bat",
                "Engine/Build/BatchFiles/RunUAT.bat",
                "Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
                "Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll"):
            path = self.engine / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(relative.encode())
        self.luac = self.root / "luac.exe"
        self.luac.write_bytes(b"synthetic luac")
        self.commit = "1" * 40
        self.commands = []

    def tearDown(self):
        self.temporary.cleanup()

    def fake_runner(self, command, cwd, log_path, timeout_seconds):
        self.commands.append(command)
        log_path.write_text(
            f"synthetic step: {Path(command[0]).name}\n",
            encoding="utf-8")
        if Path(command[0]).name.casefold() == self.luac.name.casefold():
            output = Path(command[command.index("-o") + 1])
            output.write_bytes(b"synthetic bytecode")
            return
        if Path(command[0]).name.casefold() == "build.bat":
            directory = (
                self.project / "Plugins/AkUGCEditor/Binaries/Win64")
            write_minimal_pe(
                directory / "UnrealEditor-AkUGCEditor.dll", True)
            write_json(directory / "UnrealEditor.modules", {
                "BuildId": "12345678-1234-5678-1234-123456789abc",
                "Modules": {
                    "AkUGCEditor": "UnrealEditor-AkUGCEditor.dll",
                },
            })
            return
        target_argument = next(
            value for value in command if value.startswith("-target="))
        target = target_argument.removeprefix("-target=")
        role, spec = next(
            (role, spec)
            for role, spec in build.ROLES.items()
            if spec["target"] == target)
        write_target(
            self.project / "Binaries/Win64" / f"{target}.target",
            target,
            spec["target_type"],
        )
        stage = (
            self.project / "Saved/StagedBuilds"
            / spec["stage_platform"] / "AkUGCPlatform")
        open_order = (
            self.project / "Build" / spec["stage_platform"]
            / "FileOpenOrder/CookerOpenOrder.log")
        open_order.parent.mkdir(parents=True, exist_ok=True)
        open_order.write_text("generated", encoding="utf-8")
        write_minimal_pe(
            stage / "Binaries/Win64" / spec["executable"], False)
        write_minimal_pak(
            stage / "Content/Paks" / f"AkUGCPlatform-{role}.pak")

    def run_build(self):
        output = self.root / "provenance"
        with patch.object(
                build, "capture_source_state",
                return_value=(self.commit, True)):
            report = build.run_fresh_build(
                self.project,
                self.commit,
                self.engine,
                self.luac,
                output,
                runner=self.fake_runner,
            )
        return output, report

    def test_records_clean_fresh_build_and_removes_generated_lua(self):
        output, report_path = self.run_build()
        report = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertTrue(report["local_build_provenance"])
        self.assertFalse(report["production_build_attestation"])
        self.assertFalse(report["reproducible_build_verified"])
        self.assertEqual(report["source_commit"], self.commit)
        self.assertEqual(len(report["steps"]), 5)
        self.assertEqual(len(self.commands), 5)
        self.assertFalse((self.project / "Content/Lua").exists())
        for relative in build.GENERATED_BUILD_DIRECTORIES:
            self.assertFalse((self.project / relative).exists())
        self.assertFalse((output / "fresh-build-failure.json").exists())
        for role in build.ROLES:
            staged = report["artifacts"]["roles"][role]["staged_tree"]
            self.assertEqual(len(staged["manifest_sha256"]), 64)
            self.assertGreater(staged["file_count"], 1)

    def test_rejects_existing_generated_outputs(self):
        (self.project / "Saved").mkdir()
        with patch.object(
                build, "capture_source_state",
                return_value=(self.commit, True)):
            with self.assertRaisesRegex(ValueError, "isolated worktree"):
                build.run_fresh_build(
                    self.project,
                    self.commit,
                    self.engine,
                    self.luac,
                    self.root / "provenance",
                    runner=self.fake_runner,
                )
        self.assertEqual(self.commands, [])

    def test_requires_packaged_lua_staging_rule(self):
        (self.project / "Config/DefaultGame.ini").write_text(
            "[/Script/UnrealEd.ProjectPackagingSettings]\n",
            encoding="utf-8",
        )
        with patch.object(
                build, "capture_source_state",
                return_value=(self.commit, True)):
            with self.assertRaisesRegex(ValueError, "Content/Lua"):
                build.run_fresh_build(
                    self.project,
                    self.commit,
                    self.engine,
                    self.luac,
                    self.root / "provenance",
                    runner=self.fake_runner,
                )
        self.assertEqual(self.commands, [])

    def test_rejects_dirty_or_mismatched_source(self):
        with patch.object(
                build, "capture_source_state",
                return_value=("2" * 40, True)):
            with self.assertRaisesRegex(ValueError, "clean worktree"):
                build.run_fresh_build(
                    self.project,
                    self.commit,
                    self.engine,
                    self.luac,
                    self.root / "provenance",
                    runner=self.fake_runner,
                )
        self.assertEqual(self.commands, [])


if __name__ == "__main__":
    unittest.main()
