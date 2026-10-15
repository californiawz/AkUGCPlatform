import json
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import phase1_external_acceptance as acceptance
import prepare_phase1_candidate as candidate


def write_json(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload), encoding="utf-8")


def write_minimal_pe(
        path: Path, machine: int = 0x8664, subsystem: int = 2,
        is_dll: bool = False) -> None:
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
                     machine, 1, 0, 0, 0, optional_size, characteristics)
    optional_offset = pe_offset + 24
    struct.pack_into("<H", data, optional_offset, 0x20B)
    struct.pack_into("<I", data, optional_offset + 16, 0x1000)
    struct.pack_into("<Q", data, optional_offset + 24, 0x140000000)
    struct.pack_into("<I", data, optional_offset + 32, 0x1000)
    struct.pack_into("<I", data, optional_offset + 36, 0x200)
    struct.pack_into("<I", data, optional_offset + 56, 0x2000)
    struct.pack_into("<I", data, optional_offset + 60, 0x200)
    struct.pack_into("<H", data, optional_offset + 68, subsystem)
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


def write_target_receipt(path: Path, target: str, target_type: str) -> None:
    write_json(
        path,
        {
            "TargetName": target,
            "Platform": "Win64",
            "Configuration": "Development",
            "TargetType": target_type,
            "IsTestTarget": False,
            "Architecture": "x64",
            "Launch": f"$(ProjectDir)/Binaries/Win64/{target}.exe",
            "Version": {
                "BuildId": "12345678-1234-5678-1234-123456789abc",
            },
            "BuildProducts": [
                {
                    "Path": f"$(ProjectDir)/Binaries/Win64/{target}.exe",
                    "Type": "Executable",
                }
            ],
        },
    )


def write_runtime_root(root: Path, target: str, target_type: str) -> None:
    executable = root / "Binaries" / "Win64" / f"{target}.exe"
    write_minimal_pe(executable)
    write_minimal_pak(
        root / "Content" / "Paks" / f"AkUGCPlatform-{target_type}.pak")
    (root / "Saved").mkdir()
    (root / "Saved" / "runtime-generated.log").write_text(
        "must not be archived", encoding="utf-8")


class PreparePhase1CandidateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.project = self.root / "project"
        write_json(
            self.project / "AkUGCPlatform.uproject",
            {
                "FileVersion": 3,
                "Plugins": [{"Name": "AkUGCEditor", "Enabled": True}],
            },
        )
        (self.project / "Config").mkdir()
        (self.project / "Config" / "DefaultEngine.ini").write_text(
            "[Core.System]\n", encoding="utf-8")
        (self.project / "Content").mkdir()
        (self.project / "Content" / "Marker.txt").write_text(
            "candidate", encoding="utf-8")
        (self.project / "Lua").mkdir()
        (self.project / "Lua" / "Main.lua").write_text(
            "return {}\n", encoding="utf-8")
        plugin = self.project / "Plugins" / "AkUGCEditor"
        write_json(
            plugin / "AkUGCEditor.uplugin",
            {
                "FileVersion": 3,
                "Modules": [{"Name": "AkUGCEditor", "Type": "Editor"}],
            },
        )
        (plugin / "Source").mkdir(parents=True)
        (plugin / "Source" / "Marker.cpp").write_text(
            "void CandidateMarker() {}\n", encoding="utf-8")

        self.creator_module = self.root / "build" / "UnrealEditor-AkUGCEditor.dll"
        write_minimal_pe(self.creator_module, subsystem=3, is_dll=True)
        self.creator_modules = self.root / "build" / "UnrealEditor.modules"
        write_json(
            self.creator_modules,
            {
                "BuildId": "12345678-1234-1234-1234-123456789abc",
                "Modules": {
                    "AkUGCEditor": "UnrealEditor-AkUGCEditor.dll",
                },
            },
        )
        self.game_root = self.root / "game" / "AkUGCPlatform"
        self.client_root = self.root / "client" / "AkUGCPlatform"
        self.server_root = self.root / "server" / "AkUGCPlatform"
        write_runtime_root(self.game_root, "AkUGCPlatform", "Game")
        write_runtime_root(self.client_root, "AkUGCPlatformClient", "Client")
        write_runtime_root(self.server_root, "AkUGCPlatformServer", "Server")
        self.game_target = self.root / "receipts" / "AkUGCPlatform.target"
        self.client_target = self.root / "receipts" / "AkUGCPlatformClient.target"
        self.server_target = self.root / "receipts" / "AkUGCPlatformServer.target"
        write_target_receipt(self.game_target, "AkUGCPlatform", "Game")
        write_target_receipt(
            self.client_target, "AkUGCPlatformClient", "Client")
        write_target_receipt(
            self.server_target, "AkUGCPlatformServer", "Server")
        self.commit = "1" * 40

    def tearDown(self):
        self.temporary.cleanup()

    def prepare(self, output: Path):
        with patch.object(
                candidate, "capture_source_state",
                return_value=(self.commit, True)):
            return candidate.prepare_candidate(
                self.project,
                self.commit,
                self.creator_module,
                self.creator_modules,
                self.game_root,
                self.game_target,
                self.client_root,
                self.client_target,
                self.server_root,
                self.server_target,
                output,
            )

    def test_prepares_structurally_valid_candidate_archives(self):
        output = self.root / "candidate"
        report_path = self.prepare(output)
        report = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertEqual(report_path.name, "candidate-preparation.json")
        self.assertTrue(report["local_rehearsal"])
        self.assertFalse(report["production_acceptance_evidence"])
        self.assertEqual(report["source_commit"], self.commit)
        self.assertEqual(
            report["creator_build_id"],
            "12345678-1234-1234-1234-123456789abc",
        )
        self.assertTrue((output / "baseline" / "CreatorBuild.dll").is_file())
        creator_archive = output / "baseline" / "CreatorPackage.zip"
        self.assertEqual(
            candidate.inspect_creator_package(
                creator_archive)["creator_build_id"],
            "12345678-1234-1234-1234-123456789abc",
        )
        with zipfile.ZipFile(creator_archive) as package:
            self.assertIn("AkUGCPlatform/Lua/Main.lua", package.namelist())
        for role, archive in (
                ("game_package", "GamePackage.zip"),
                ("client_package", "ClientPackage.zip"),
                ("server_package", "ServerPackage.zip")):
            info = candidate.inspect_runtime_package(
                output / "baseline" / archive, role)
            self.assertEqual(
                info["archive_sha256"],
                report["artifacts"][role]["sha256"],
            )
            with zipfile.ZipFile(output / "baseline" / archive) as package:
                names = package.namelist()
                self.assertIn(
                    "AkUGCPlatform/AkUGCPlatform.uproject", names)
                self.assertFalse(any(
                    name.startswith("AkUGCPlatform/Saved/")
                    for name in names))

    def test_rejects_private_keys_and_removes_partial_output(self):
        private_key = self.project / "Content" / "developer.key"
        private_key.write_text("secret", encoding="utf-8")
        output = self.root / "candidate"

        with self.assertRaisesRegex(ValueError, "private-key"):
            self.prepare(output)
        self.assertFalse(output.exists())
        self.assertEqual(
            list(self.root.glob("candidate.preparing-*")),
            [],
        )

    def test_rejects_protected_publisher_identity(self):
        identity = self.project / "Content" / "publisher-identity.json"
        identity.write_text(
            '{"version":1,"protection":"windows-dpapi-current-user-v1"}',
            encoding="utf-8",
        )
        output = self.root / "candidate"

        with self.assertRaisesRegex(ValueError, "private-key"):
            self.prepare(output)
        self.assertFalse(output.exists())

    def test_rejects_stale_publisher_identity_temporary(self):
        identity = (
            self.project / "Content" /
            "publisher-identity.json.deadbeef.tmp"
        )
        identity.write_text("encrypted-credential-fragment", encoding="utf-8")
        output = self.root / "candidate"

        with self.assertRaisesRegex(ValueError, "private-key"):
            self.prepare(output)
        self.assertFalse(output.exists())

    def test_rejects_encrypted_publisher_recovery_bundle(self):
        recovery = self.project / "Content" / "author.akugcrecovery"
        recovery.write_text(
            '{"protection":"scrypt-aes-256-gcm-v1","ciphertext":"secret"}',
            encoding="utf-8",
        )
        output = self.root / "candidate"

        with self.assertRaisesRegex(ValueError, "private-key"):
            self.prepare(output)
        self.assertFalse(output.exists())

    def test_rejects_stale_recovery_bundle_temporary(self):
        recovery = (
            self.project / "Content" /
            "author.akugcrecovery.deadbeef.tmp"
        )
        recovery.write_text("encrypted-credential-fragment", encoding="utf-8")
        output = self.root / "candidate"

        with self.assertRaisesRegex(ValueError, "private-key"):
            self.prepare(output)
        self.assertFalse(output.exists())

    def test_requires_clean_matching_source_commit(self):
        output = self.root / "candidate"
        with patch.object(
                candidate, "capture_source_state",
                return_value=("2" * 40, True)):
            with self.assertRaisesRegex(ValueError, "project_root"):
                candidate.prepare_candidate(
                    self.project,
                    self.commit,
                    self.creator_module,
                    self.creator_modules,
                    self.game_root,
                    self.game_target,
                    self.client_root,
                    self.client_target,
                    self.server_root,
                    self.server_target,
                    output,
                )
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
