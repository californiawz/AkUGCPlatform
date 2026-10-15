import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid
import zipfile
from unittest.mock import patch

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import phase1_external_acceptance as acceptance


class ExternalAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def evidence(self, name, content=None):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        data = content if isinstance(content, bytes) else (content or name).encode()
        path.write_bytes(data)
        return path.relative_to(self.root).as_posix()

    @staticmethod
    def digest(value):
        return hashlib.sha256(value).hexdigest()

    @staticmethod
    def pe_image(dll):
        payload = bytearray(8192)
        payload[:2] = b"MZ"
        payload[0x3c:0x40] = (0x80).to_bytes(4, "little")
        payload[0x80:0x84] = b"PE\0\0"
        payload[0x84:0x86] = (0x8664).to_bytes(2, "little")
        payload[0x86:0x88] = (1).to_bytes(2, "little")
        payload[0x94:0x96] = (0xF0).to_bytes(2, "little")
        characteristics = 0x0002 | (0x2000 if dll else 0)
        payload[0x96:0x98] = characteristics.to_bytes(2, "little")
        payload[0x98:0x9a] = (0x20B).to_bytes(2, "little")
        payload[0xa8:0xac] = (0x1000).to_bytes(4, "little")
        payload[0xb0:0xb8] = (0x140000000).to_bytes(8, "little")
        payload[0xb8:0xbc] = (0x1000).to_bytes(4, "little")
        payload[0xbc:0xc0] = (0x200).to_bytes(4, "little")
        payload[0xd0:0xd4] = (0x2000).to_bytes(4, "little")
        payload[0xd4:0xd8] = (0x200).to_bytes(4, "little")
        payload[0xdc:0xde] = (3).to_bytes(2, "little")
        payload[0x104:0x108] = (16).to_bytes(4, "little")
        section = 0x188
        payload[section:section + 8] = b".text\0\0\0"
        payload[section + 8:section + 12] = (0x200).to_bytes(4, "little")
        payload[section + 12:section + 16] = (0x1000).to_bytes(4, "little")
        payload[section + 16:section + 20] = (0x200).to_bytes(4, "little")
        payload[section + 20:section + 24] = (0x200).to_bytes(4, "little")
        payload[section + 36:section + 40] = (0x60000020).to_bytes(4, "little")
        payload[0x200] = 0xC3
        return bytes(payload)

    @staticmethod
    def pak_image():
        index = b"synthetic-index" * 20
        body = index + b"P" * (4096 - len(index))
        footer = (
            b"\0" * 16
            + b"\0"
            + acceptance.PAK_MAGIC
            + acceptance.PAK_VERSION_LATEST.to_bytes(4, "little", signed=True)
            + (0).to_bytes(8, "little", signed=True)
            + len(index).to_bytes(8, "little", signed=True)
            + hashlib.sha1(index).digest()
            + b"\0" * acceptance.PAK_COMPRESSION_METHOD_BYTES
        )
        return body + footer

    @staticmethod
    def project_descriptor():
        return json.dumps({
            "FileVersion": 3,
            "Plugins": [{"Name": "AkUGCEditor", "Enabled": True}],
        }).encode()

    @staticmethod
    def plugin_descriptor():
        return json.dumps({
            "FileVersion": 3,
            "Modules": [{"Name": "AkUGCEditor", "Type": "Editor"}],
        }).encode()

    @staticmethod
    def target_receipt(executable, target_type):
        target_name = executable.removesuffix(".exe")
        executable_path = f"$(ProjectDir)/Binaries/Win64/{executable}"
        return json.dumps({
            "TargetName": target_name,
            "Platform": "Win64",
            "Configuration": "Development",
            "TargetType": target_type,
            "IsTestTarget": False,
            "Architecture": "x64",
            "Launch": executable_path,
            "Version": {"BuildId": "12345678-1234-5678-1234-567812345678"},
            "BuildProducts": [{"Path": executable_path, "Type": "Executable"}],
        }).encode()

    def creator_entries(self, creator_data, extra=None):
        entries = {
            "AkUGCPlatform.uproject": self.project_descriptor(),
            "Config/DefaultEngine.ini": b"[/Script/Engine.Engine]",
            "Content/dummy.uasset": b"content",
            "Plugins/AkUGCEditor/AkUGCEditor.uplugin": self.plugin_descriptor(),
            "Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor.modules": json.dumps({
                "BuildId": "12345678-1234-5678-1234-567812345678",
                "Modules": {
                    "AkUGCEditor": "UnrealEditor-AkUGCEditor.dll",
                },
            }).encode(),
            "Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor-AkUGCEditor.dll":
                creator_data,
        }
        entries.update(extra or {})
        return entries

    def runtime_entries(self, executable, target_type, overrides=None):
        target_name = executable.removesuffix(".exe")
        entries = {
            "AkUGCPlatform/AkUGCPlatform.uproject": json.dumps({
                "FileVersion": 3,
            }).encode(),
            f"AkUGCPlatform/Binaries/Win64/{executable}": self.pe_image(False),
            f"AkUGCPlatform/Binaries/Win64/{target_name}.target":
                self.target_receipt(executable, target_type),
            "AkUGCPlatform/Content/Paks/AkUGCPlatform.pak":
                self.pak_image(),
        }
        entries.update(overrides or {})
        return entries

    def archive(self, name, entries):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
            for entry_name, content in entries.items():
                archive.writestr(entry_name, content)
        return path.relative_to(self.root).as_posix(), self.digest(path.read_bytes())

    def work_package(self, name, release_id, private_key, payload_marker):
        manifest = {
            "releaseId": release_id,
            "schemaVersion": acceptance.PRODUCT_SCHEMA_VERSION,
            "projectId": "12345678-1234-5678-1234-567812345678",
            "templateId": "TowerDefense",
            "capabilities": ["logic", "ruleset", "world.spawn", "rules.wave"],
            "assetDependencies": [],
            "contentHash": self.digest(payload_marker.encode()),
        }
        canonical_manifest = {
            "assetdependencies": manifest["assetDependencies"],
            "capabilities": [item.lower() for item in manifest["capabilities"]],
            "contenthash": manifest["contentHash"],
            "projectid": manifest["projectId"],
            "releaseid": str(uuid.UUID(release_id)).lower(),
            "schemaversion": manifest["schemaVersion"],
            "templateid": manifest["templateId"].lower(),
        }
        message = json.dumps(
            canonical_manifest, ensure_ascii=True, separators=(",", ":"),
            sort_keys=True).encode("utf-8")
        public_key = private_key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
        signature = {
            "algorithm": "ed25519",
            "publicKey": public_key,
            "signature": private_key.sign(message).hex(),
        }
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        package = {
            "manifest": manifest,
            "document": {"marker": payload_marker},
            "program": {"source": f"return '{payload_marker}'"},
            "signature": signature,
        }
        path.write_text(json.dumps(
            package, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        return path.relative_to(self.root).as_posix(), self.digest(path.read_bytes())

    def valid_manifest(self):
        manifest = acceptance.make_template("a" * 40)
        creator_data = self.pe_image(True)
        creator_file = self.evidence("baseline/creator.dll", creator_data)
        creator_package_file, creator_package_hash = self.archive(
            "baseline/creator.zip", self.creator_entries(creator_data))
        game_file, game_hash = self.archive(
            "baseline/game.zip", self.runtime_entries("AkUGCPlatform.exe", "Game"))
        client_file, client_hash = self.archive(
            "baseline/client.zip",
            self.runtime_entries("AkUGCPlatformClient.exe", "Client"))
        server_file, server_hash = self.archive(
            "baseline/server.zip",
            self.runtime_entries("AkUGCPlatformServer.exe", "Server"))
        manifest["baseline"].update({
            "creator_build_file": creator_file,
            "creator_build_sha256": self.digest(creator_data),
            "creator_package_file": creator_package_file,
            "creator_package_sha256": creator_package_hash,
            "game_package_file": game_file,
            "game_package_sha256": game_hash,
            "client_package_file": client_file,
            "client_package_sha256": client_hash,
            "server_package_file": server_file,
            "server_package_sha256": server_hash,
        })
        private_key = Ed25519PrivateKey.generate()
        public_key = private_key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
        slider_evidence = {
            "target": "wave_count",
            "target_verified": True,
            "capture_type": "SSpinBox<int32>",
            "start_waves": 3,
            "captured_waves": 2,
            "released_waves": 2,
            "nonzero_delta_events": 3,
            "high_precision_delta_events": 1,
            "released": True,
            "keyboard_value_input": False,
        }
        checkpoint_tags = [
            "new", "slider", "draft", "saved-draft", "parameters-applied",
            "parameters-undo", "parameters-redo", "saved-applied", "reloaded", "discarded",
        ]
        manual = {
            "kind": "OS_mouse_keyboard_creator_parameters",
            "passed": True,
            "manual_slider": True,
            "numeric_slider_verified": True,
            "slider_input_mode": "awaited_physical_mouse",
            "human_ui_validated": False,
            "physical_input_provenance_verified": False,
            "automated_os_input": True,
            "automated_slider_motion": False,
            "acceptance_nonce": manifest["session_nonce"],
            "window_title": f"AkUGC Physical Slider [{manifest['session_nonce']}]",
            "creator_build_sha256": manifest["baseline"]["creator_build_sha256"],
            "creator_build_unchanged_during_run": True,
            "checkpoint_count": 10,
            "expected_checkpoints": 10,
            "checkpoints": [
                {
                    "tag": tag,
                    "passed": True,
                    "acceptance_nonce": manifest["session_nonce"],
                    "creator_build_sha256": manifest["baseline"]["creator_build_sha256"],
                    **({"slider_evidence": slider_evidence} if tag == "slider" else {}),
                }
                for tag in checkpoint_tags
            ],
            "slider_evidence": slider_evidence,
            "actions": [
                {
                    "action": "awaiting_physical_mouse",
                    "target": "wave_count",
                    "acceptance_nonce": manifest["session_nonce"],
                    "creator_build_sha256": manifest["baseline"]["creator_build_sha256"],
                },
                {"action": "numeric_slider_evidence_validated", "target": "wave_count"},
            ],
        }
        manual_path = self.evidence("usability/manual-slider.json", json.dumps(manual).encode())
        manifest["physical_slider"].update({
            "tester_alias": "tester-a",
            "observer_alias": "observer-a",
            "observed_at_utc": "2026-10-14T01:55:00Z",
            "evidence_capture_consent": True,
            "used_physical_mouse": True,
            "automated_slider_motion_disabled": True,
            "manual_slider_report": manual_path,
            "evidence_files": [
                self.evidence("usability/slider-before.png"),
                self.evidence("usability/slider-after.png"),
            ],
            "observer_attested": True,
            "notes": "The tester moved the physical mouse.",
        })
        first_release = "f" * 32
        first_work_path, first_work_hash = self.work_package(
            "usability/first-work.akugc", first_release, private_key, "first-work")
        usability_evidence = [
            self.evidence(f"usability/checkpoint-{index}.png", f"checkpoint-{index}".encode())
            for index in range(4)
        ]
        manifest["usability"].update({
            "tester_alias": "tester-a",
            "observer_alias": "observer-a",
            "tester_is_non_programmer": True,
            "evidence_capture_consent": True,
            "started_at_utc": "2026-10-14T01:00:00Z",
            "finished_at_utc": "2026-10-14T01:45:00Z",
            "used_logic_graph": False,
            "created_project": True,
            "saved_project": True,
            "published_work": True,
            "exported_share": True,
            "recipient_imported_and_played": True,
            "independent_key_channel_verified": True,
            "creator_device": "creator-pc",
            "recipient_device": "recipient-pc",
            "creator_package_sha256": manifest["baseline"]["creator_package_sha256"],
            "trusted_public_key": public_key,
            "share_file": first_work_path,
            "share_sha256": first_work_hash,
            "evidence_files": usability_evidence,
            "observer_attested": True,
            "notes": "Observed without coaching.",
        })

        manifest["cross_device"]["seed_runs"] = []
        manifest["cross_device"]["resilience_runs"] = []
        for run_index in range(10):
            release_id = f"{run_index + 1:032x}"
            content_hash = self.digest(f"seed-{run_index}".encode())
            share_path, share_hash = self.work_package(
                f"seeds/seed-{run_index}.akugc", release_id, private_key, f"seed-{run_index}")
            player_count = 2 if run_index % 2 == 0 else 4
            host_mode = "listen" if run_index % 2 == 0 else "dedicated"
            host_device = "device-1" if host_mode == "listen" else "server-pc"
            participants = []
            for player_index in range(player_count):
                role = "game" if host_mode == "listen" and player_index == 0 else "client"
                participants.append({
                    "device": f"device-{player_index + 1}",
                    "package_role": role,
                    "package_sha256": manifest["baseline"][
                        "game_package_sha256" if role == "game" else "client_package_sha256"],
                    "release_id": release_id,
                    "content_hash": content_hash,
                    "result": "Victory",
                    "team_score": 1400 + run_index,
                    "base_health": 790,
                    "resources": 90,
                    "log": self.evidence(
                        f"seeds/seed-{run_index}-device-{player_index + 1}.log",
                        f"log-{run_index}-{player_index}".encode(),
                    ),
                    "screenshot": self.evidence(
                        f"seeds/seed-{run_index}-device-{player_index + 1}.png",
                        f"png-{run_index}-{player_index}".encode(),
                    ),
                })
            tampered_copy = ""
            tampered_copy_hash = ""
            tamper_rejection = ""
            if run_index == 0:
                tampered = json.loads((self.root / share_path).read_text(encoding="utf-8"))
                tampered["document"]["marker"] = "seed-0-tampered"
                tampered_copy = self.evidence(
                    "seeds/seed-0-tampered.akugc", json.dumps(tampered).encode())
                tampered_copy_hash = self.digest((self.root / tampered_copy).read_bytes())
                rejection_report = {
                    "tampered_share_sha256": tampered_copy_hash,
                    "release_id": release_id,
                    "content_hash": content_hash,
                    "rejected_by_product_loader": True,
                }
                tamper_rejection = self.evidence(
                    "seeds/seed-0-tamper-rejected.json",
                    json.dumps(rejection_report).encode())
            host_observation = {"participant_index": 0}
            if host_mode == "dedicated":
                host_observation = {
                    "device": host_device,
                    "package_role": "server",
                    "package_sha256": manifest["baseline"]["server_package_sha256"],
                    "release_id": release_id,
                    "content_hash": content_hash,
                    "result": "Victory",
                    "team_score": 1400 + run_index,
                    "base_health": 790,
                    "resources": 90,
                    "log": self.evidence(
                        f"seeds/seed-{run_index}-authority.log",
                        f"authority-{run_index}".encode()),
                }
            manifest["cross_device"]["seed_runs"].append({
                "seed_id": f"seed-{run_index + 1}",
                "release_id": release_id,
                "trusted_public_key": public_key,
                "share_file": share_path,
                "share_sha256": share_hash,
                "transferred_between_devices": True,
                "independent_key_channel_verified": True,
                "tampered_copy_rejected": run_index == 0,
                "tampered_copy_file": tampered_copy,
                "tampered_copy_sha256": tampered_copy_hash,
                "tamper_rejection_evidence": tamper_rejection,
                "host_mode": host_mode,
                "host_device": host_device,
                "host_package_sha256": (
                    manifest["baseline"]["game_package_sha256"]
                    if host_mode == "listen" else manifest["baseline"]["server_package_sha256"]
                ),
                "host_observation": host_observation,
                "participants": participants,
                "observer_alias": "observer-a",
                "observed_at_utc": f"2026-10-14T02:{run_index:02d}:00Z",
                "observer_attested": True,
                "notes": "",
            })

        for case in sorted(acceptance.RESILIENCE_CASES):
            host_mode = "listen" if case == "host_terminated" else "dedicated"
            host_device = "device-1" if host_mode == "listen" else "server-pc"
            host_role = "game" if host_mode == "listen" else "server"
            manifest["cross_device"]["resilience_runs"].append({
                "case": case,
                "observer_alias": "observer-a",
                "observed_at_utc": "2026-10-14T02:30:00Z",
                "host_mode": host_mode,
                "host_device": host_device,
                "host_package_sha256": manifest["baseline"][
                    "game_package_sha256" if host_mode == "listen" else "server_package_sha256"],
                "fault_device": host_device if case == "host_terminated" else "device-2",
                "recovery_device": host_device if case == "host_terminated" else "device-2",
                "fault_evidence": self.evidence(
                    f"resilience/{case}-fault.png", f"{case}-fault".encode()),
                "recovery_evidence": self.evidence(
                    f"resilience/{case}-recovery.png", f"{case}-recovery".encode()),
                "fault_action_observed": True,
                "net_driver_cleared": True,
                "roster_cleared": True,
                "session_cleared": True,
                "recovered_to_clean_state": True,
                "followup_play_succeeded": True,
                "new_process_used": case in {"host_terminated", "reconnect"},
                "device_reports": [
                    {
                        "device": host_device,
                        "package_role": host_role,
                        "package_sha256": manifest["baseline"][f"{host_role}_package_sha256"],
                        "before_process_instance": f"{case}-host-before",
                        "after_process_instance": (
                            f"{case}-host-after" if case == "host_terminated"
                            else f"{case}-host-before"),
                        "log": self.evidence(f"resilience/{case}-{host_device}.log"),
                    },
                    {
                        "device": "device-2",
                        "package_role": "client",
                        "package_sha256": manifest["baseline"]["client_package_sha256"],
                        "before_process_instance": f"{case}-client-before",
                        "after_process_instance": (
                            f"{case}-client-after" if case == "reconnect"
                            else f"{case}-client-before"),
                        "log": self.evidence(f"resilience/{case}-device-2.log"),
                    },
                ],
                "observer_attested": True,
                "notes": "",
            })

        attestation_data = b"signed external acceptance attestation"
        manifest["final_attestation"].update({
            "coordinator_alias": "coordinator-a",
            "devices_are_physical_and_distinct": True,
            "real_network_used": True,
            "real_file_transfer_used": True,
            "same_machine_processes_not_counted": True,
            "claims_are_human_attested_not_software_proven": True,
            "evidence_unmodified": True,
            "signed_at_utc": "2026-10-14T03:00:00Z",
            "attestation_file": self.evidence(
                "attestation/coordinator-signoff.pdf", attestation_data),
            "attestation_sha256": self.digest(attestation_data),
            "signed_off": True,
        })
        return manifest

    def validate(self, manifest):
        return acceptance.validate_manifest(manifest, self.root)

    def test_complete_human_attested_evidence_passes_without_overclaim(self):
        result = self.validate(self.valid_manifest())
        self.assertTrue(result["passed"], result["diagnostics"])
        self.assertTrue(result["human_attested"])
        self.assertFalse(result["software_independently_verified_human_claims"])
        self.assertEqual(result["metrics"]["duration_minutes"], 45)
        self.assertTrue(result["metrics"]["physical_slider_observed"])
        self.assertEqual(result["metrics"]["seed_count"], 10)
        self.assertEqual(result["metrics"]["player_counts"], [2, 4])
        self.assertEqual(result["metrics"]["host_modes"], ["dedicated", "listen"])
        self.assertEqual(set(result["metrics"]["resilience_cases"]), acceptance.RESILIENCE_CASES)
        self.assertGreater(result["metrics"]["evidence_file_count"], 80)

    def test_usability_time_logic_and_manual_slider_fail_closed(self):
        manifest = self.valid_manifest()
        manifest["usability"]["finished_at_utc"] = "2026-10-14T02:01:00Z"
        manifest["usability"]["used_logic_graph"] = True
        report = self.root / manifest["physical_slider"]["manual_slider_report"]
        value = json.loads(report.read_text())
        value["slider_input_mode"] = "native_shift_accelerated_os_input"
        report.write_text(json.dumps(value), encoding="utf-8")
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertTrue(any("within 60 minutes" in item for item in result["diagnostics"]))
        self.assertTrue(any("used_logic_graph" in item for item in result["diagnostics"]))
        self.assertTrue(any("manual slider report" in item for item in result["diagnostics"]))

    def test_seed_identity_package_and_final_state_mismatches_fail(self):
        manifest = self.valid_manifest()
        first, second = manifest["cross_device"]["seed_runs"][:2]
        second["release_id"] = first["release_id"]
        second["share_sha256"] = first["share_sha256"]
        second["participants"][0]["package_sha256"] = "e" * 64
        second["participants"][0]["content_hash"] = "d" * 64
        second["participants"][0]["team_score"] += 1
        first_log = self.root / first["participants"][0]["log"]
        second_log = self.root / second["participants"][0]["log"]
        second_log.write_bytes(first_log.read_bytes())
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertTrue(any("release IDs must be unique" in item for item in result["diagnostics"]))
        self.assertTrue(any("share hashes must be unique" in item for item in result["diagnostics"]))
        self.assertTrue(any("frozen role baseline" in item for item in result["diagnostics"]))
        self.assertTrue(any("content_hash differs" in item for item in result["diagnostics"]))
        self.assertTrue(any("disagree on team_score" in item for item in result["diagnostics"]))
        self.assertTrue(any("must not reuse identical evidence" in item for item in result["diagnostics"]))

    def test_seed_count_player_coverage_tamper_and_resilience_are_required(self):
        manifest = self.valid_manifest()
        manifest["cross_device"]["seed_runs"] = manifest["cross_device"]["seed_runs"][:9]
        for run in manifest["cross_device"]["seed_runs"]:
            run["participants"] = run["participants"][:2]
            run["tampered_copy_rejected"] = False
        manifest["cross_device"]["resilience_runs"] = manifest["cross_device"]["resilience_runs"][:1]
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        joined = "\n".join(result["diagnostics"])
        self.assertIn("at least 10 seed runs", joined)
        self.assertIn("both 2-player and 4-player", joined)
        self.assertIn("tampered-copy rejection", joined)
        self.assertIn("missing cross-device resilience cases", joined)

    def test_missing_traversal_and_private_key_evidence_are_rejected(self):
        manifest = self.valid_manifest()
        manifest["usability"]["evidence_files"][0] = "../outside.png"
        private_key = self.evidence("usability/private.key", b"do-not-collect")
        manifest["physical_slider"]["manual_slider_report"] = private_key
        manifest["cross_device"]["seed_runs"][0]["participants"][0]["log"] = "missing.log"
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        joined = "\n".join(result["diagnostics"])
        self.assertIn("escapes the evidence directory", joined)
        self.assertIn("must not collect a private key", joined)
        self.assertIn("does not exist", joined)

    def test_candidate_archives_require_formats_exclude_keys_and_bind_snapshot(self):
        manifest = self.valid_manifest()
        creator_data = (self.root / manifest["baseline"]["creator_build_file"]).read_bytes()
        _, creator_hash = self.archive(
            manifest["baseline"]["creator_package_file"],
            self.creator_entries(
                creator_data, {"Saved/UGC/Publisher/private.key": b"publisher-secret"}))
        manifest["baseline"]["creator_package_sha256"] = creator_hash
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertIn("contains a private-key file", "\n".join(result["diagnostics"]))

        manifest = self.valid_manifest()
        creator_data = (self.root / manifest["baseline"]["creator_build_file"]).read_bytes()
        _, creator_hash = self.archive(
            manifest["baseline"]["creator_package_file"],
            self.creator_entries(
                creator_data, {
                    "Saved/UGC/Publisher/publisher-identity.json":
                        b'{"protectedPrivateKey":"encrypted-secret"}',
                }))
        manifest["baseline"]["creator_package_sha256"] = creator_hash
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertIn("contains a private-key file", "\n".join(result["diagnostics"]))

        manifest = self.valid_manifest()
        creator_data = (self.root / manifest["baseline"]["creator_build_file"]).read_bytes()
        _, creator_hash = self.archive(
            manifest["baseline"]["creator_package_file"],
            self.creator_entries(
                creator_data, {
                    "Saved/UGC/Publisher/"
                    "publisher-identity.json.deadbeef.tmp":
                        b"encrypted-credential-fragment",
                }))
        manifest["baseline"]["creator_package_sha256"] = creator_hash
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertIn("contains a private-key file", "\n".join(result["diagnostics"]))

        manifest = self.valid_manifest()
        game_file = manifest["baseline"]["game_package_file"]
        _, game_hash = self.archive(
            game_file,
            self.runtime_entries(
                "AkUGCPlatform.exe", "Game", {
                    "AkUGCPlatform/Binaries/Win64/AkUGCPlatform.exe": b"not-a-pe",
                }))
        manifest["baseline"]["game_package_sha256"] = game_hash
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertIn("Windows PE image", "\n".join(result["diagnostics"]))

        malformed_pe = bytearray(self.pe_image(False))
        malformed_pe[0xa8:0xac] = (0x3000).to_bytes(4, "little")
        malformed_pe[0x194:0x198] = (0x3000).to_bytes(4, "little")
        manifest = self.valid_manifest()
        game_file = manifest["baseline"]["game_package_file"]
        _, game_hash = self.archive(
            game_file,
            self.runtime_entries(
                "AkUGCPlatform.exe", "Game", {
                    "AkUGCPlatform/Binaries/Win64/AkUGCPlatform.exe":
                        bytes(malformed_pe),
                }))
        manifest["baseline"]["game_package_sha256"] = game_hash
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertIn("PE section table", "\n".join(result["diagnostics"]))

        manifest = self.valid_manifest()
        game_file = manifest["baseline"]["game_package_file"]
        _, game_hash = self.archive(
            game_file,
            self.runtime_entries(
                "AkUGCPlatform.exe", "Client", {
                    "AkUGCPlatform/Content/Paks/AkUGCPlatform.pak": b"X" * 4096,
                }))
        manifest["baseline"]["game_package_sha256"] = game_hash
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        joined = "\n".join(result["diagnostics"])
        self.assertIn("does not describe AkUGCPlatform Win64 Game", joined)

        manifest = self.valid_manifest()
        game_file = manifest["baseline"]["game_package_file"]
        incomplete_footer = (
            b"P" * 4096
            + acceptance.PAK_MAGIC
            + acceptance.PAK_VERSION_LATEST.to_bytes(4, "little", signed=True)
            + (0).to_bytes(8, "little", signed=True)
            + (1).to_bytes(8, "little", signed=True)
            + b"H" * 20
        )
        _, game_hash = self.archive(
            game_file,
            self.runtime_entries(
                "AkUGCPlatform.exe", "Game", {
                    "AkUGCPlatform/Content/Paks/AkUGCPlatform.pak":
                        incomplete_footer,
                }))
        manifest["baseline"]["game_package_sha256"] = game_hash
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertIn("Pak info footer", "\n".join(result["diagnostics"]))

        manifest = self.valid_manifest()
        original_inspector = acceptance.inspect_runtime_package

        def inspect_with_wrong_snapshot(path, role):
            inspected = original_inspector(path, role)
            if role == "game_package":
                inspected["archive_sha256"] = "0" * 64
            return inspected

        with patch.object(
                acceptance, "inspect_runtime_package",
                side_effect=inspect_with_wrong_snapshot):
            result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertIn(
            "inspection snapshot differs from its frozen SHA-256",
            "\n".join(result["diagnostics"]))

    def test_missing_and_unknown_fields_are_rejected(self):
        manifest = self.valid_manifest()
        del manifest["baseline"]["server_package_sha256"]
        manifest["final_attestation"]["automatic_truth_proof"] = True
        manifest["cross_device"]["seed_runs"][0]["seed_id"] = []
        manifest["cross_device"]["seed_runs"][0]["host_mode"] = []
        manifest["cross_device"]["seed_runs"][0]["participants"][0]["package_role"] = []
        manifest["cross_device"]["seed_runs"][0]["participants"][0]["team_score"] = []
        manifest["cross_device"]["resilience_runs"][0]["device_reports"][0][
            "package_role"] = []
        report = self.root / manifest["physical_slider"]["manual_slider_report"]
        manual = json.loads(report.read_text())
        manual["actions"].append({"action": []})
        report.write_text(json.dumps(manual), encoding="utf-8")
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        joined = "\n".join(result["diagnostics"])
        self.assertIn("baseline missing fields", joined)
        self.assertIn("final_attestation has unknown fields", joined)

    def test_malformed_resilience_case_fails_without_crashing(self):
        manifest = self.valid_manifest()
        manifest["cross_device"]["resilience_runs"][0]["case"] = []
        report = self.root / manifest["physical_slider"]["manual_slider_report"]
        report.write_text("[]", encoding="utf-8")
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertTrue(any(".case must be" in item for item in result["diagnostics"]))
        self.assertTrue(any(
            "manual_slider_report must contain a JSON object" in item
            for item in result["diagnostics"]))

    def test_signed_package_signature_release_and_payload_checks_fail_closed(self):
        manifest = self.valid_manifest()
        first, second, third, fourth = manifest["cross_device"]["seed_runs"][:4]

        first_path = self.root / first["share_file"]
        first_package = json.loads(first_path.read_text(encoding="utf-8"))
        signature = first_package["signature"]["signature"]
        first_package["signature"]["signature"] = (
            signature[:-1] + ("0" if signature[-1] != "0" else "1"))
        first_path.write_text(json.dumps(first_package), encoding="utf-8")
        first["share_sha256"] = self.digest(first_path.read_bytes())

        second["release_id"] = "e" * 32

        second_path = self.root / second["share_file"]
        third_path = self.root / third["share_file"]
        second_package = json.loads(second_path.read_text(encoding="utf-8"))
        third_package = json.loads(third_path.read_text(encoding="utf-8"))
        third_package["document"] = second_package["document"]
        third_package["program"] = second_package["program"]
        third_path.write_text(json.dumps(third_package), encoding="utf-8")
        third["share_sha256"] = self.digest(third_path.read_bytes())

        fourth_path = self.root / fourth["share_file"]
        fourth_package = json.loads(fourth_path.read_text(encoding="utf-8"))
        fourth_package["manifest"]["schemaVersion"] = 1
        fourth_path.write_text(json.dumps(fourth_package), encoding="utf-8")
        fourth["share_sha256"] = self.digest(fourth_path.read_bytes())

        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        joined = "\n".join(result["diagnostics"])
        self.assertIn("Ed25519 signature is invalid", joined)
        self.assertIn("release_id differs from the signed package manifest", joined)
        self.assertIn("seed document/program payloads must be valid and unique", joined)
        self.assertIn("work package manifest value types are invalid", joined)

    def test_host_builds_must_match_game_and_server_baselines(self):
        manifest = self.valid_manifest()
        manifest["cross_device"]["seed_runs"][0]["host_package_sha256"] = "0" * 64
        manifest["cross_device"]["resilience_runs"][0]["host_package_sha256"] = "1" * 64
        terminated = next(
            run for run in manifest["cross_device"]["resilience_runs"]
            if run["case"] == "host_terminated")
        terminated["host_mode"] = "dedicated"
        terminated["host_package_sha256"] = manifest["baseline"]["server_package_sha256"]
        terminated["device_reports"][0]["package_role"] = "server"
        terminated["device_reports"][0]["package_sha256"] = manifest["baseline"][
            "server_package_sha256"]
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        self.assertGreaterEqual(sum(
            "differs from the frozen Game/Server baseline" in item
            for item in result["diagnostics"]), 2)
        self.assertTrue(any(
            "host_terminated must exercise a Listen host" in item
            for item in result["diagnostics"]))

    def test_human_roles_and_evidence_cannot_be_reused(self):
        manifest = self.valid_manifest()
        manifest["usability"]["observer_alias"] = manifest["usability"]["tester_alias"]
        reused = manifest["physical_slider"]["evidence_files"][0]
        manifest["usability"]["evidence_files"][0] = reused
        result = self.validate(manifest)
        self.assertFalse(result["passed"])
        joined = "\n".join(result["diagnostics"])
        self.assertIn("tester and observer aliases must represent separate roles", joined)
        self.assertIn("reuses evidence already assigned", joined)

    def test_cli_rejects_duplicate_json_fields_and_requires_immutable_lock(self):
        duplicate = self.root / "duplicate.json"
        duplicate.write_text(
            '{"schema_version":1,"schema_version":1}', encoding="utf-8")
        duplicate_result = self.root / "duplicate-result.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(acceptance.main([
                "validate", "--input", str(duplicate), "--output", str(duplicate_result),
            ]), 1)
        self.assertIn(
            "Cannot read acceptance manifest",
            "\n".join(json.loads(duplicate_result.read_text())["diagnostics"]))
        for value in ("NaN", "Infinity", "-Infinity", "1e999"):
            with self.subTest(non_finite=value), self.assertRaises(ValueError):
                acceptance.strict_json_loads('{"value":' + value + "}")

        deep = self.root / "deep.json"
        deep.write_text("[" * 2000 + "0" + "]" * 2000, encoding="utf-8")
        deep_result = self.root / "deep-result.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(acceptance.main([
                "validate", "--input", str(deep), "--output", str(deep_result),
            ]), 1)
        self.assertFalse(json.loads(deep_result.read_text())["passed"])

        manifest_path = self.root / "acceptance.json"
        acceptance.write_json(manifest_path, self.valid_manifest())
        unsealed_result = self.root / "unsealed-result.json"
        with patch.object(
                acceptance, "capture_source_state", return_value=("b" * 40, True)):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([
                    "seal", "--input", str(manifest_path),
                ]), 1)
        with patch.object(
                acceptance, "capture_source_state", return_value=("a" * 40, False)):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([
                    "seal", "--input", str(manifest_path),
                ]), 1)
        self.assertFalse((self.root / "evidence-lock.json").exists())
        with patch.object(
                acceptance, "capture_source_state", return_value=("a" * 40, True)):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([
                    "validate", "--input", str(manifest_path), "--output",
                    str(unsealed_result),
                ]), 1)
                self.assertEqual(acceptance.main([
                    "seal", "--input", str(manifest_path),
                ]), 0)
        self.assertFalse(json.loads(unsealed_result.read_text())["passed"])
        with patch.object(
                acceptance, "capture_source_state", return_value=("a" * 40, True)):
            with self.assertRaises(RuntimeError):
                acceptance.main(["seal", "--input", str(manifest_path)])
        with self.assertRaises(RuntimeError):
            acceptance.main([
                "validate", "--input", str(manifest_path),
                "--output", str(self.root / "evidence-lock.json"),
            ])

        evidence = self.root / json.loads(
            manifest_path.read_text())["usability"]["evidence_files"][0]
        evidence.write_bytes(b"modified-after-seal")
        tampered_result = self.root / "tampered-result.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(acceptance.main([
                "validate", "--input", str(manifest_path), "--output", str(tampered_result),
            ]), 1)
        self.assertIn(
            "evidence lock does not match",
            "\n".join(json.loads(tampered_result.read_text())["diagnostics"]))

        lock_path = self.root / "evidence-lock.json"
        lock_path.write_text("[]", encoding="utf-8")
        malformed_lock_result = self.root / "malformed-lock-result.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(acceptance.main([
                "validate", "--input", str(manifest_path),
                "--output", str(malformed_lock_result),
            ]), 1)
        self.assertIn(
            "evidence lock does not match",
            "\n".join(json.loads(malformed_lock_result.read_text())["diagnostics"]))

    def test_init_is_fresh_and_requires_full_commit(self):
        source_state = patch.object(
            acceptance, "capture_source_state", return_value=("a" * 40, True))
        source_state.start()
        self.addCleanup(source_state.stop)
        output = self.root / "acceptance.json"
        creator = self.root / "creator.dll"
        creator_package = self.root / "creator.zip"
        game = self.root / "game.zip"
        client = self.root / "client.zip"
        server = self.root / "server.zip"
        creator_data = self.pe_image(True)
        creator.write_bytes(creator_data)
        self.archive("creator.zip", self.creator_entries(creator_data))
        self.archive("game.zip", self.runtime_entries("AkUGCPlatform.exe", "Game"))
        self.archive(
            "client.zip", self.runtime_entries("AkUGCPlatformClient.exe", "Client"))
        self.archive(
            "server.zip", self.runtime_entries("AkUGCPlatformServer.exe", "Server"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(acceptance.main([
                "init", "--output", str(output), "--source-commit", "a" * 40,
                "--creator-build", str(creator), "--creator-package", str(creator_package),
                "--game-package", str(game),
                "--client-package", str(client),
                "--server-package", str(server),
            ]), 0)
        initialized = json.loads(output.read_text())
        baseline = initialized["baseline"]
        self.assertEqual(baseline["source_commit"], "a" * 40)
        self.assertEqual(baseline["client_package_sha256"], self.digest(client.read_bytes()))
        self.assertEqual(len(initialized["cross_device"]["seed_runs"]), 10)
        self.assertEqual(len(initialized["cross_device"]["resilience_runs"]), 4)
        with patch.object(
                acceptance, "capture_source_state", return_value=("b" * 40, False)):
            with self.assertRaises(RuntimeError):
                acceptance.main([
                    "init", "--output", str(self.root / "dirty.json"),
                    "--source-commit", "a" * 40,
                    "--creator-build", str(creator),
                    "--creator-package", str(creator_package),
                    "--game-package", str(game), "--client-package", str(client),
                    "--server-package", str(server),
                ])
        with self.assertRaises(RuntimeError):
            acceptance.main([
                "init", "--output", str(output), "--source-commit", "a" * 40,
                "--creator-build", str(creator), "--creator-package", str(creator_package),
                "--game-package", str(game),
                "--client-package", str(client), "--server-package", str(server),
            ])
        with self.assertRaises(RuntimeError):
            acceptance.main([
                "init", "--output", str(self.root / "other.json"), "--source-commit", "short",
                "--creator-build", str(creator), "--creator-package", str(creator_package),
                "--game-package", str(game),
                "--client-package", str(client), "--server-package", str(server),
            ])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            acceptance.main(["init", "--output", str(self.root / "partial.json")])

    def test_source_state_treats_nonignored_untracked_files_as_dirty(self):
        responses = [
            subprocess.CompletedProcess([], 0, stdout="a" * 40 + "\n"),
            subprocess.CompletedProcess([], 0, stdout="?? stray-source.cpp\n"),
        ]
        with patch.object(acceptance.subprocess, "run", side_effect=responses) as runner:
            commit, clean = acceptance.capture_source_state(self.root)
        self.assertEqual(commit, "a" * 40)
        self.assertFalse(clean)
        self.assertIn("--untracked-files=all", runner.call_args_list[1].args[0])
        self.assertIn("--ignore-submodules=none", runner.call_args_list[1].args[0])

    def test_cli_validate_writes_result_for_success_and_malformed_input(self):
        manifest_path = self.root / "acceptance.json"
        acceptance.write_json(manifest_path, self.valid_manifest())
        result_path = self.root / "validated.json"
        with patch.object(
                acceptance, "capture_source_state", return_value=("a" * 40, True)):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main([
                    "seal", "--input", str(manifest_path),
                ]), 0)
                self.assertEqual(acceptance.main([
                    "validate", "--input", str(manifest_path), "--output", str(result_path),
                ]), 0)
        self.assertTrue(json.loads(result_path.read_text())["passed"])

        malformed = self.root / "malformed.json"
        malformed.write_text("{", encoding="utf-8")
        malformed_result = self.root / "malformed-result.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(acceptance.main([
                "validate", "--input", str(malformed), "--output", str(malformed_result),
            ]), 1)
        self.assertFalse(json.loads(malformed_result.read_text())["passed"])


if __name__ == "__main__":
    unittest.main()
