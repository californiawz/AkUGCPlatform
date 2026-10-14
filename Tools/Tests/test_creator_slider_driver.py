import contextlib
import copy
import ctypes
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import verify_copilot_http as driver


ROOT = Path(__file__).resolve().parents[2]
VALID_EVIDENCE = {
    "target": "wave_count", "target_verified": True, "capture_type": "SSpinBox<NumericType>",
    "start_waves": 3, "captured_waves": 2, "released_waves": 2,
    "nonzero_delta_events": 4, "high_precision_delta_events": 4,
    "released": True, "keyboard_value_input": False,
}


class SliderEvidenceTests(unittest.TestCase):
    def test_valid_observed_slate_evidence(self):
        self.assertTrue(driver.validate_slider_evidence(VALID_EVIDENCE))
        self.assertEqual(driver.slider_evidence_diagnostics(VALID_EVIDENCE), [])

    def test_numeric_json_values_may_be_floats(self):
        evidence = dict(VALID_EVIDENCE, start_waves=3.0, captured_waves=2.0, released_waves=2.0)
        self.assertTrue(driver.validate_slider_evidence(evidence))

    def test_only_os_input_sent_is_not_evidence(self):
        self.assertFalse(driver.validate_slider_evidence({"action": "numeric_slider_drag", "os_input_sent": True}))

    def test_zero_high_precision_delta_fails(self):
        self.assertFalse(driver.validate_slider_evidence(dict(VALID_EVIDENCE, high_precision_delta_events=0)))

    def test_zero_nonzero_delta_fails(self):
        self.assertFalse(driver.validate_slider_evidence(dict(VALID_EVIDENCE, nonzero_delta_events=0)))

    def test_wrong_capture_type_target_or_identity_fails(self):
        for field, value in (("capture_type", "SEditableText"), ("capture_type", None),
                             ("target", "initial_resources"), ("target_verified", False)):
            with self.subTest(field=field, value=value):
                self.assertFalse(driver.validate_slider_evidence(dict(VALID_EVIDENCE, **{field: value})))

    def test_all_required_fields_are_required(self):
        for field in VALID_EVIDENCE:
            with self.subTest(field=field):
                evidence = dict(VALID_EVIDENCE)
                del evidence[field]
                self.assertFalse(driver.validate_slider_evidence(evidence))

    def test_wrong_start_captured_and_released_values_fail(self):
        for field in ("start_waves", "captured_waves", "released_waves"):
            for value in (0, 1, None, "2", True):
                with self.subTest(field=field, value=value):
                    self.assertFalse(driver.validate_slider_evidence(dict(VALID_EVIDENCE, **{field: value})))

    def test_keyboard_value_input_and_unreleased_fail(self):
        for field, value in (("keyboard_value_input", True), ("released", False),
                             ("released", 1), ("target_verified", 1), ("keyboard_value_input", 0)):
            with self.subTest(field=field, value=value):
                self.assertFalse(driver.validate_slider_evidence(dict(VALID_EVIDENCE, **{field: value})))

    def test_invalid_event_counts_fail(self):
        for field in ("nonzero_delta_events", "high_precision_delta_events"):
            for value in (-1, "1", True, None, float("nan"), float("inf")):
                with self.subTest(field=field, value=value):
                    self.assertFalse(driver.validate_slider_evidence(dict(VALID_EVIDENCE, **{field: value})))

    def test_nonobjects_fail(self):
        for value in (None, [], "", 3):
            self.assertFalse(driver.validate_slider_evidence(value))


class FreshControlHitTests(unittest.TestCase):
    def setUp(self):
        self.control = {"type": "SNumericEntryBox<int32>", "label": "波数（1–3，默认 3）",
                        "x": 100, "y": 200, "width": 80, "height": 20, "enabled": True}
        self.state = {"controls": [self.control], "cursor_x": 100, "cursor_y": 200,
                      "hover_path": [self.control["type"] + ":" + self.control["label"], "SSpinBox<int32>:"]}

    def test_fresh_geometry_cursor_and_spinbox_match(self):
        self.assertTrue(driver.fresh_control_hit(self.state, self.control, "波数", True))

    def test_stale_cursor_even_with_hover_path_fails(self):
        self.assertFalse(driver.fresh_control_hit(dict(self.state, cursor_x=300), self.control, "波数", True))

    def test_changed_geometry_fails(self):
        state = dict(self.state, controls=[dict(self.control, x=105)])
        self.assertFalse(driver.fresh_control_hit(state, self.control, "波数", True))

    def test_missing_hover_or_spinbox_fails(self):
        self.assertFalse(driver.fresh_control_hit(dict(self.state, hover_path=[]), self.control, "波数"))
        state = dict(self.state, hover_path=self.state["hover_path"][:1])
        self.assertFalse(driver.fresh_control_hit(state, self.control, "波数", True))

    def test_disabled_or_missing_cursor_fails(self):
        state = copy.deepcopy(self.state)
        del state["cursor_x"]
        self.assertFalse(driver.fresh_control_hit(state, self.control, "波数"))
        self.control["enabled"] = False
        self.assertFalse(driver.fresh_control_hit(self.state, self.control, "波数"))

    def test_menu_hit_allows_bounded_nonclient_frame_offset_only_when_requested(self):
        control = {"type": "STextBlock", "label": "窗口", "x": 272.5, "y": 17.5,
                   "width": 40, "height": 23, "enabled": True}
        state = {"controls": [control], "cursor_x": 263, "cursor_y": 6,
                 "hover_path": ["SButton:窗口", "STextBlock:窗口"]}
        self.assertFalse(driver.fresh_control_hit(state, control, "窗口"))
        self.assertTrue(driver.fresh_control_hit(
            state, control, "窗口", allow_cursor_within_bounds=True))
        self.assertFalse(driver.fresh_control_hit(
            dict(state, cursor_y=4), control, "窗口", allow_cursor_within_bounds=True))


class DriverCliTests(unittest.TestCase):
    def parse(self, *arguments):
        return driver.parse_arguments(["--engine", "E:/unused-engine", *arguments], ROOT)

    def assert_cli_error(self, *arguments):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failure:
            self.parse(*arguments)
        self.assertEqual(failure.exception.code, 2)

    def test_slider_requires_mouse_and_parameters(self):
        for mode in ("--slider-ui", "--manual-slider"):
            self.assert_cli_error(mode)
            self.assert_cli_error("--mouse-ui", mode)
            self.assert_cli_error("--parameter-ui", mode)
            extra = ["--acceptance-nonce", "a" * 32] if mode == "--manual-slider" else []
            args = self.parse("--mouse-ui", "--parameter-ui", mode, *extra)
            self.assertTrue(args.parameter_ui)

    def test_slider_modes_are_mutually_exclusive(self):
        self.assert_cli_error("--mouse-ui", "--parameter-ui", "--slider-ui", "--manual-slider")

    def test_acceptance_nonce_is_required_and_manual_only(self):
        self.assert_cli_error("--mouse-ui", "--parameter-ui", "--manual-slider")
        self.assert_cli_error(
            "--mouse-ui", "--parameter-ui", "--manual-slider",
            "--acceptance-nonce", "not-a-nonce")
        self.assert_cli_error("--acceptance-nonce", "a" * 32)
        args = self.parse(
            "--mouse-ui", "--parameter-ui", "--manual-slider",
            "--acceptance-nonce", "A" * 32)
        self.assertEqual(args.acceptance_nonce, "a" * 32)

    def test_mouse_modes_run_separately(self):
        for mode in ("--menu-entry", "--library-ui", "--mouse-live-authorized"):
            self.assert_cli_error("--mouse-ui", "--parameter-ui", mode)

    def test_all_ui_modes_require_mouse_ui(self):
        for mode in ("--parameter-ui", "--menu-entry", "--library-ui", "--mouse-live-authorized"):
            self.assert_cli_error(mode)

    def test_mouse_and_headless_cannot_mix(self):
        self.assert_cli_error("--mouse-ui", "--all-tests")
        self.assert_cli_error("--mouse-ui", "--generated-document", str(ROOT / "Saved/nonexistent.json"))

    def test_live_guard_without_reading_real_environment(self):
        with patch.dict("os.environ", {}, clear=True):
            self.assert_cli_error("--mouse-ui", "--mouse-live-authorized")

    def test_output_must_be_fresh_and_strictly_under_saved(self):
        for path in (ROOT, ROOT / "Saved", ROOT / "Content/new-run", ROOT / "Saved/../Content/new-run"):
            self.assert_cli_error("--output", str(path))
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            parent = Path(temporary)
            self.assert_cli_error("--output", str(parent))
            existing = parent / "existing.json"
            existing.write_text("{}", encoding="utf-8")
            self.assert_cli_error("--output", str(existing))
            fresh = parent / "fresh"
            args = self.parse("--output", str(fresh))
            self.assertEqual(args.output, fresh.resolve())
            self.assertFalse(fresh.exists())

    def test_generated_document_protections(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            document = Path(temporary) / "document.json"
            document.write_text("{}", encoding="utf-8")
            self.assert_cli_error("--generated-document", str(document))
            self.assert_cli_error("--all-tests", "--generated-document", str(document.with_name("missing.json")))
            self.assert_cli_error("--all-tests", "--generated-document", str(ROOT / "AkUGCPlatform.uproject"))
            args = self.parse("--all-tests", "--generated-document", str(document))
            self.assertEqual(args.generated_document, document.resolve())

    def test_parameter_and_library_paths_are_isolated(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            for mode in ("--parameter-ui", "--library-ui"):
                run = Path(temporary) / mode[2:]
                args = self.parse("--mouse-ui", mode, "--output", str(run))
                output, user_dir = driver.verification_paths(args, ROOT)
                self.assertEqual(user_dir, run / "editor-user")
                self.assertEqual(output, user_dir / "Saved/verification")
                self.assertTrue(output.is_dir())
                with self.assertRaises(FileExistsError):
                    driver.verification_paths(args, ROOT)

    def test_headless_output_is_not_relocated(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            run = Path(temporary) / "fresh"
            args = self.parse("--output", str(run))
            output, _ = driver.verification_paths(args, ROOT)
            self.assertEqual(output, run)

    def test_command_has_isolation_slider_flags_and_no_simulation(self):
        for mode in (None, "--slider-ui", "--manual-slider"):
            flags = ["--mouse-ui", "--parameter-ui"] + ([mode] if mode else [])
            if mode == "--manual-slider":
                flags += ["--acceptance-nonce", "a" * 32]
            args = self.parse(*flags)
            command = driver.mouse_ui_command(
                ["unused-editor", "project", "-nullrhi", "-unattended", "-ExecCmds=old"], args,
                ROOT / "Saved/output", ROOT / "Saved/user")
            self.assertIn("-CreatorParameterMouse", command)
            self.assertIn("-UserDir=" + str(ROOT / "Saved/user"), command)
            self.assertIn("-UnattendedInput", command)
            self.assertNotIn("-unattended", command)
            self.assertNotIn("-nullrhi", command)
            self.assertNotIn("ForceRawInputSimulation", " ".join(command))
            self.assertEqual("-CreatorSliderMouse" in command, mode is not None)
            self.assertEqual("-CreatorManualSlider" in command, mode == "--manual-slider")
            self.assertEqual(
                any(part == "-CreatorAcceptanceNonce=" + "a" * 32 for part in command),
                mode == "--manual-slider")
            self.assertEqual(
                any(part.startswith("-CreatorBuildSha256=") for part in command),
                mode == "--manual-slider")


class FocusTests(unittest.TestCase):
    def test_menu_popup_from_same_process_retains_input_ownership(self):
        owners = {123: 456, 124: 456, 999: 777}
        owner = owners.__getitem__
        self.assertTrue(driver.owns_input_foreground(123, 456, False, lambda: 123, owner))
        self.assertFalse(driver.owns_input_foreground(123, 456, False, lambda: 124, owner))
        self.assertTrue(driver.owns_input_foreground(123, 456, True, lambda: 124, owner))
        self.assertFalse(driver.owns_input_foreground(123, 456, True, lambda: 999, owner))

    def test_bounded_recovery_only_targets_owned_window(self):
        clock = [0.0]
        focus = Mock(return_value=999)
        set_focus = Mock()
        with self.assertRaises(RuntimeError):
            driver.recover_initial_focus(123, 456, focus, lambda _: 456, set_focus,
                                         clock=lambda: clock[0], sleep=lambda seconds: clock.__setitem__(0, clock[0] + seconds))
        self.assertLessEqual(set_focus.call_count, 51)
        self.assertGreaterEqual(clock[0], 5)
        self.assertTrue(all(call.args == (123,) for call in set_focus.call_args_list))

    def test_foreign_owner_never_receives_focus(self):
        set_focus = Mock()
        with self.assertRaises(RuntimeError):
            driver.recover_initial_focus(123, 456, lambda: 999, lambda _: 777, set_focus)
        set_focus.assert_not_called()

    def test_already_owned_foreground_is_not_reactivated(self):
        set_focus = Mock()
        driver.recover_initial_focus(123, 456, lambda: 123, lambda _: 456, set_focus)
        set_focus.assert_not_called()

    def test_focus_loss_during_operation_stops_without_refocus_or_input(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            output = Path(temporary)
            state = {"controls": [{"type": "SButton", "label": "新建默认可玩塔防", "x": 200,
                                   "y": 200, "width": 100, "height": 30, "enabled": True}],
                     "window_x": 0, "window_y": 0, "window_width": 1000, "window_height": 800,
                     "cursor_x": 0, "cursor_y": 0, "hover_path": []}
            driver.write_json(output / "ui-state.json", state)
            process = Mock(pid=456)
            process.poll.return_value = None
            user = Mock()
            active = [123]
            user.FindWindowW.return_value = 123
            user.GetForegroundWindow.side_effect = lambda: active[0]
            user.GetWindowThreadProcessId.side_effect = lambda handle, pid: setattr(pid._obj, "value", 456 if handle == 123 else 777)

            def get_rect(_, pointer):
                rect = pointer._obj
                rect.left, rect.top, rect.right, rect.bottom = 0, 0, 1000, 800
                return 1

            def lose_focus(*_):
                active[0] = 999
                return 1

            user.GetWindowRect.side_effect = get_rect
            user.SetCursorPos.side_effect = lose_focus
            with patch.object(ctypes, "WinDLL", return_value=user, create=True):
                with self.assertRaises(RuntimeError):
                    driver.drive_mouse_ui(process, output, "")
            user.SetForegroundWindow.assert_not_called()
            user.mouse_event.assert_not_called()
            user.keybd_event.assert_not_called()
            user.SendInput.assert_not_called()
            self.assertEqual(driver.read_json(output / "os-input-actions.json")[0]["action"], "initial_focus_verified")


class JournalAndResultTests(unittest.TestCase):
    def test_actions_are_incremental_and_retained_on_failure(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            output = Path(temporary)

            def fail(*arguments):
                actions = arguments[-1]
                actions.append({"action": "relative_mouse_move_sent", "dx": -12})
                self.assertEqual(len(driver.read_json(output / "os-input-actions.json")), 1)
                raise RuntimeError("test failure")

            with patch.object(driver, "_drive_mouse_ui", side_effect=fail), self.assertRaises(RuntimeError):
                driver.drive_mouse_ui(Mock(), output, "", parameters=True, slider=True)
            self.assertEqual(len(driver.read_json(output / "os-input-actions.json")), 1)

    def test_actions_cannot_set_numeric_verified(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            output = Path(temporary)
            args = driver.parse_arguments(["--engine", "unused", "--mouse-ui", "--parameter-ui", "--slider-ui"])
            driver.write_json(output / "os-input-actions.json", [{"action": "numeric_slider_drag"}])
            self.assertFalse(driver.slider_result_details(output, args, completed=True)["numeric_slider_verified"])
            driver.write_json(output / "slider-evidence.json", VALID_EVIDENCE)
            self.assertFalse(driver.slider_result_details(output, args)["numeric_slider_verified"])
            self.assertTrue(driver.slider_result_details(output, args, completed=True)["numeric_slider_verified"])
            self.assertFalse(driver.slider_result_details(output, args, completed=True)["human_ui_validated"])

    def test_result_distinguishes_shift_acceleration_from_physical_unmodified_acceptance(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            output = Path(temporary)
            args = driver.parse_arguments(["--engine", "unused", "--mouse-ui", "--parameter-ui", "--slider-ui"])
            driver.write_json(output / "os-input-actions.json", [
                {"action": "modifier_down", "modifier": "LeftShift", "target": "wave_count"}])
            result = driver.slider_result_details(output, args, completed=True)
            self.assertTrue(result["native_shift_acceleration_used"])
            self.assertEqual(result["slider_input_mode"], "native_shift_accelerated_os_input")
            self.assertFalse(result["physical_input_provenance_verified"])
            self.assertFalse(result["unmodified_physical_slider_verified"])
            self.assertFalse(result["numeric_slider_verified"])
            driver.write_json(output / "slider-evidence.json", VALID_EVIDENCE)
            result = driver.slider_result_details(output, args, completed=True)
            self.assertTrue(result["numeric_slider_verified"])
            self.assertFalse(result["human_ui_validated"])
            args.slider_ui, args.manual_slider = False, True
            args.acceptance_nonce = "a" * 32
            args.creator_build_sha256 = driver.creator_build_sha256(ROOT)
            result = driver.slider_result_details(output, args, completed=True)
            self.assertFalse(result["native_shift_acceleration_used"])
            self.assertEqual(result["slider_input_mode"], "awaited_physical_mouse")
            self.assertEqual(result["acceptance_nonce"], "a" * 32)
            self.assertRegex(result["creator_build_sha256"], r"^[0-9a-f]{64}$")

    def test_main_failure_reports_slider_diagnostics_and_actions(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            run = Path(temporary) / "fresh"

            def fail(args, root, output, user_dir):
                actions = driver.ActionJournal(output / "os-input-actions.json")
                actions.append({"action": "relative_mouse_move_sent", "dx": -12})
                driver.write_json(output / "ui-state.json", {"slider_evidence": dict(VALID_EVIDENCE, high_precision_delta_events=0)})
                raise RuntimeError("DO_NOT_LOG_EXCEPTION_INPUT")

            with patch.object(driver, "_run_verification", side_effect=fail), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(RuntimeError):
                    driver.main(["--engine", "unused", "--mouse-ui", "--parameter-ui", "--slider-ui", "--output", str(run)])
            output = run / "editor-user/Saved/verification"
            result = driver.read_json(output / "result.json")
            self.assertFalse(result["passed"])
            self.assertFalse(result["numeric_slider_verified"])
            self.assertTrue(result["slider_diagnostics"])
            self.assertEqual(len(result["actions"]), 1)
            self.assertNotIn("DO_NOT_LOG_EXCEPTION_INPUT", (output / "result.json").read_text(encoding="utf-8"))
            self.assertEqual(result["actions"], driver.read_json(output / "os-input-actions.json"))

    def test_parameter_mode_does_not_create_servers_or_certificates(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            output = Path(temporary)
            args = driver.parse_arguments(["--engine", "unused", "--mouse-ui", "--parameter-ui"])
            with patch.object(driver, "ThreadingHTTPServer") as server, patch.object(driver.rsa, "generate_private_key") as rsa:
                with patch.object(driver.subprocess, "Popen", side_effect=RuntimeError("mock launch failure")) as popen:
                    with self.assertRaises(RuntimeError):
                        driver._run_verification(args, ROOT, output, output / "editor-user")
                server.assert_not_called()
                rsa.assert_not_called()
                self.assertNotIn("CopilotTls", " ".join(popen.call_args.args[0]))
                self.assertFalse((output / "fixture-ca.pem").exists())

    def test_manual_process_wait_uses_420_second_budget(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            output = Path(temporary)
            args = driver.parse_arguments([
                "--engine", "unused", "--mouse-ui", "--parameter-ui", "--manual-slider",
                "--acceptance-nonce", "a" * 32,
            ])
            process = Mock(pid=456)
            process.poll.return_value = 0
            process.wait.return_value = 0

            def complete(*arguments):
                self.assertTrue(arguments[-1])
                (output / "editor.log").write_text("", encoding="utf-8")
                driver.write_json(output / "ui-result.json", {"passed": True})
                driver.write_json(output / "os-input-actions.json", [])
                driver.write_json(output / "slider-evidence.json", VALID_EVIDENCE)

            with patch.object(driver.subprocess, "Popen", return_value=process), patch.object(driver, "drive_mouse_ui", side_effect=complete):
                with patch.object(driver.time, "monotonic", return_value=10), contextlib.redirect_stdout(io.StringIO()):
                    driver._run_verification(args, ROOT, output, output / "editor-user")
            process.wait.assert_called_once_with(timeout=420)
            process.terminate.assert_not_called()
            self.assertTrue(driver.read_json(output / "result.json")["numeric_slider_verified"])


class SliderFlowTests(unittest.TestCase):
    def exercise_until_slider(self, manual, evidence=None, lose_focus_on_release=False,
                              lose_focus_after_mouse_up=False, fail_during_move=False):
        # All OS and Slate responses below are fixtures; no Editor or physical input is used.
        import os
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            output = Path(temporary)
            wave = {"type": "SNumericEntryBox<int32>", "label": "波数（1–3，默认 3）",
                    "x": 200, "y": 250, "width": 80, "height": 24, "enabled": True}
            new = {"type": "SButton", "label": "新建默认可玩塔防", "x": 200, "y": 150,
                   "width": 100, "height": 30, "enabled": True}
            state = {"controls": [new, wave], "window_x": 0, "window_y": 0,
                     "window_width": 1000, "window_height": 800, "cursor_x": 0, "cursor_y": 0,
                     "window_title": (
                         "AkUGC Physical Slider [" + "a" * 32 + "]" if manual
                         else "AkUGC Copilot OS Mouse Verification"),
                     "acceptance_nonce": "a" * 32,
                     "creator_build_sha256": "b" * 64,
                     "hover_path": [], "project_open": False, "draft_waves": 3, "draft_resources": 300,
                     "draft_base_health": 1000, "document_revision": 1, "can_undo": False,
                     "can_redo": False, "mouse_captured": False,
                     "project_path": str(output.parent / "project.akugc")}
            write, read = driver.write_json, driver.read_json
            versions = [0]
            clock, active = [0.0], [123]
            tags, wait_start, observed_ready = [], [], []

            def persist_state():
                write(output / "ui-state.json", state)
                path = output / "ui-state.json"
                versions[0] = max(versions[0] + 10000, path.stat().st_mtime_ns)
                os.utime(path, ns=(versions[0], versions[0]))

            persist_state()
            write(output / "ui-ack.json", {"ack": 0, "checkpoint_passed": True})
            process = Mock(pid=456)
            process.poll.return_value = None
            user = Mock()
            user.FindWindowW.return_value = 123
            user.GetForegroundWindow.side_effect = lambda: active[0]
            user.GetWindowThreadProcessId.side_effect = lambda handle, pid: setattr(pid._obj, "value", 456 if handle == 123 else 777)

            def get_rect(_, pointer):
                rect = pointer._obj
                rect.left, rect.top, rect.right, rect.bottom = 0, 0, 1000, 800
                return 1

            def position(x, y):
                state["cursor_x"], state["cursor_y"] = x, y
                c = new if y == new["y"] else wave
                state["hover_path"] = [c["type"] + ":" + c["label"], "SSpinBox<int32>:"]
                persist_state()
                return 1

            user.input_events = []
            shift = [False]

            def modifier(vk, scan, flags, extra):
                user.input_events.append(("modifier", vk, flags))
                self.assertEqual((vk, scan, extra), (0xA0, 0x2A, 0))
                shift[0] = flags == 0

            def mouse(flags, *_):
                user.input_events.append(("mouse", flags, shift[0]))
                if flags == 0x0002 and state["cursor_y"] == new["y"]:
                    state["project_open"] = True
                    persist_state()
                if flags == 0x0001:
                    self.assertTrue(shift[0])
                    if fail_during_move:
                        raise RuntimeError("mock motion failure")
                if flags == 0x0001 and not manual and evidence is not None:
                    state["slider_evidence"] = dict(evidence, released=False, released_waves=0)
                    state["draft_waves"] = evidence["captured_waves"]
                    state["mouse_captured"] = True
                    persist_state()
                if flags == 0x0004 and state["cursor_y"] == wave["y"]:
                    self.assertTrue(shift[0])
                    if not manual and evidence is not None:
                        state["slider_evidence"] = dict(evidence)
                        state["draft_waves"] = evidence["released_waves"]
                        state["mouse_captured"] = False
                        persist_state()
                    if lose_focus_after_mouse_up:
                        active[0] = 999
                if flags == 0x0001 and lose_focus_on_release:
                    active[0] = 999

            user.keybd_event.side_effect = modifier

            def json_read(path):
                if path.name == "ui-ack.json":
                    command = read(output / "ui-command.json")
                    tag = command["tag"]
                    tags.append(tag)
                    if tag == "slider":
                        raise RuntimeError("stopped at verified slider checkpoint")
                    return {"ack": command["seq"], "checkpoint_passed": True}
                return read(path)

            def json_write(path, value):
                write(path, value)
                if path.name == "slider-ready.json":
                    observed_ready.append(value)
                    wait_start.append(clock[0])
                    if evidence is not None:
                        state["slider_evidence"] = dict(evidence)
                        state["draft_waves"] = 2
                        persist_state()

            user.GetWindowRect.side_effect = get_rect
            user.SetCursorPos.side_effect = position
            user.mouse_event.side_effect = mouse
            with patch.object(ctypes, "WinDLL", return_value=user, create=True), patch.object(driver, "read_json", side_effect=json_read):
                with patch.object(driver, "write_json", side_effect=json_write), patch.object(driver.time, "monotonic", side_effect=lambda: clock[0]):
                    with patch.object(driver.time, "sleep", side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
                        with contextlib.redirect_stdout(io.StringIO()):
                            with self.assertRaises((RuntimeError, TimeoutError)) as error:
                                driver.drive_mouse_ui(process, output, "", parameters=True, slider=not manual, manual_slider=manual)
            if manual:
                user.keybd_event.assert_not_called()
            else:
                self.assertGreaterEqual(user.keybd_event.call_count, 1)
                self.assertTrue(all(call.args[0] == 0xA0 for call in user.keybd_event.call_args_list))
            user.SendInput.assert_not_called()
            user.SetForegroundWindow.assert_not_called()
            actions = read(output / "os-input-actions.json")
            latched = read(output / "slider-evidence.json")
            return user, tags, observed_ready, actions, latched, error.exception, clock[0] - (wait_start[0] if wait_start else 0)

    def test_manual_waits_for_full_evidence_without_injecting_wave_input(self):
        user, tags, ready, actions, evidence, error, _ = self.exercise_until_slider(True, VALID_EVIDENCE)
        self.assertEqual(tags, ["new", "slider"])
        self.assertEqual(ready[0]["phase"], "awaiting_physical_mouse")
        self.assertEqual(ready[0]["pid"], 456)
        self.assertEqual(ready[0]["target_geometry"]["label"], "波数（1–3，默认 3）")
        self.assertEqual(user.SetCursorPos.call_count, 1)  # New-project button only.
        self.assertEqual([call.args[0] for call in user.mouse_event.call_args_list], [0x0002, 0x0004])
        self.assertTrue(driver.validate_slider_evidence(evidence))
        self.assertIn("verified slider checkpoint", str(error))
        self.assertFalse(any(action["action"] == "keyboard_input" for action in actions))

    def test_manual_invalid_high_precision_evidence_waits_240_seconds_then_fails(self):
        user, tags, _, _, evidence, error, elapsed = self.exercise_until_slider(
            True, dict(VALID_EVIDENCE, high_precision_delta_events=0))
        self.assertEqual(tags, ["new"])
        self.assertIsInstance(error, TimeoutError)
        self.assertGreaterEqual(elapsed, 240)
        self.assertLess(elapsed, 241)
        self.assertFalse(driver.validate_slider_evidence(evidence))
        self.assertEqual(user.mouse_event.call_count, 2)

    def test_auto_os_sent_without_slate_delta_fails_and_does_not_keyboard_override(self):
        user, tags, ready, actions, evidence, error, _ = self.exercise_until_slider(False)
        self.assertIsInstance(error, TimeoutError)
        self.assertEqual(tags, ["new"])
        self.assertEqual(ready, [])
        self.assertTrue(any(action["action"] == "relative_mouse_move_sent" for action in actions))
        self.assertFalse(driver.validate_slider_evidence(evidence))
        self.assertEqual(user.mouse_event.call_args_list[-1].args[0], 0x0004)

    def test_auto_stops_at_two_then_requires_actual_release_evidence(self):
        user, tags, _, actions, evidence, error, _ = self.exercise_until_slider(False, VALID_EVIDENCE)
        self.assertEqual(tags, ["new", "slider"])
        self.assertEqual(sum(a["action"] == "relative_mouse_move_sent" for a in actions), 1)
        self.assertTrue(driver.validate_slider_evidence(evidence))
        self.assertEqual(user.mouse_event.call_args_list[-1].args[0], 0x0004)
        self.assertIn("verified slider checkpoint", str(error))
        self.assertFalse(next(a for a in actions if a["action"] == "numeric_slider_motion_plan")
                         ["physical_input_provenance_verified"])

    def test_auto_captured_two_with_wrong_released_value_still_fails(self):
        _, tags, _, actions, evidence, error, _ = self.exercise_until_slider(
            False, dict(VALID_EVIDENCE, released_waves=3))
        self.assertIsInstance(error, TimeoutError)
        self.assertEqual(tags, ["new"])
        self.assertEqual(sum(a["action"] == "relative_mouse_move_sent" for a in actions), 1)
        self.assertFalse(driver.validate_slider_evidence(evidence))

    def test_auto_high_precision_events_without_value_change_still_fail(self):
        _, tags, _, actions, evidence, error, elapsed = self.exercise_until_slider(
            False, dict(VALID_EVIDENCE, captured_waves=3, released_waves=3,
                        nonzero_delta_events=19, high_precision_delta_events=19))
        self.assertIsInstance(error, TimeoutError)
        self.assertEqual(tags, ["new"])
        self.assertEqual(sum(a["action"] == "relative_mouse_move_sent" for a in actions), 80)
        self.assertFalse(driver.validate_slider_evidence(evidence))
        self.assertLess(elapsed, 12)

    def test_focus_loss_never_injects_mouse_release_into_other_process(self):
        user, tags, _, actions, _, error, _ = self.exercise_until_slider(False, lose_focus_on_release=True)
        self.assertIsInstance(error, RuntimeError)
        self.assertEqual(tags, ["new"])
        self.assertEqual(user.mouse_event.call_args_list[-1].args[0], 0x0001)
        self.assertTrue(any(action["action"] == "release_skipped_after_focus_loss" for action in actions))
        user.keybd_event.assert_called_once_with(0xA0, 0x2A, 0, 0)
        self.assertTrue(any(a["action"] == "modifier_release_skipped_after_focus_loss" for a in actions))

    def test_shift_remains_held_through_mouse_up_then_releases(self):
        user, _, _, actions, _, _, _ = self.exercise_until_slider(False, VALID_EVIDENCE)
        self.assertEqual(user.input_events[-2:], [("mouse", 0x0004, True), ("modifier", 0xA0, 2)])
        self.assertEqual([a["action"] for a in actions if a["action"].startswith("modifier")],
                         ["modifier_down", "modifier_up"])
        self.assertFalse(VALID_EVIDENCE["keyboard_value_input"])

    def test_modifier_is_released_in_finally_after_motion_error(self):
        user, tags, _, actions, _, error, _ = self.exercise_until_slider(False, fail_during_move=True)
        self.assertIn("mock motion failure", str(error))
        self.assertEqual(tags, ["new"])
        self.assertEqual(user.input_events[-2:], [("mouse", 0x0004, True), ("modifier", 0xA0, 2)])
        self.assertTrue(any(a["action"] == "modifier_up" for a in actions))

    def test_focus_lost_after_mouse_up_skips_modifier_release(self):
        user, tags, _, actions, _, error, _ = self.exercise_until_slider(
            False, VALID_EVIDENCE, lose_focus_after_mouse_up=True)
        self.assertIsInstance(error, RuntimeError)
        self.assertEqual(tags, ["new"])
        self.assertEqual(user.input_events[-1], ("mouse", 0x0004, True))
        user.keybd_event.assert_called_once_with(0xA0, 0x2A, 0, 0)
        self.assertTrue(any(a["action"] == "modifier_release_skipped_after_focus_loss" for a in actions))


class StartupFocusTests(unittest.TestCase):
    def setUp(self):
        self.process = Mock(pid=9876)
        self.process.poll.return_value = None
        self.clock = 0.0
        self.active = 111
        self.actions = []
        self.candidate = {"window": 123, "pid": self.process.pid, "visible": True,
                          "width": 1600, "height": 900, "title": "AkUGCPlatform - Unreal Editor"}
        self.enumerate = Mock(return_value=[self.candidate])
        self.read = Mock(return_value={})
        self.owner = Mock(side_effect=lambda handle: self.process.pid if handle == 123 else 444)
        self.focus = Mock(side_effect=self.acquire)

    def acquire(self, handle):
        self.active = handle
        return 1

    def sleep(self, seconds):
        self.clock += seconds

    def wait(self):
        return driver.wait_for_startup_controls(
            self.process, self.read, self.enumerate, lambda: self.active,
            self.owner, self.focus, self.actions, clock=lambda: self.clock, sleep=self.sleep)

    def test_no_window_times_out_at_100_seconds_without_activation(self):
        self.enumerate.return_value = []
        with self.assertRaises(TimeoutError):
            self.wait()
        self.assertEqual(self.clock, 100)
        self.assertEqual(self.enumerate.call_count, 200)
        self.focus.assert_not_called()
        self.assertEqual(self.actions, [])

    def test_wrong_pid_never_receives_startup_focus(self):
        self.candidate["pid"] = 2222
        with self.assertRaises(TimeoutError):
            self.wait()
        self.focus.assert_not_called()
        self.assertEqual(self.actions, [])

    def test_only_visible_large_editor_titles_are_eligible(self):
        for changes in ({"title": "Another application"}, {"visible": False}, {"width": 800},
                        {"height": 600}, {"title": "Unreal Editor dialog", "width": 1200}):
            with self.subTest(changes=changes):
                self.clock = 0.0
                self.enumerate.return_value = [dict(self.candidate, **changes)]
                with self.assertRaises(TimeoutError):
                    self.wait()
                self.focus.assert_not_called()
        self.assertEqual(self.actions, [])

    def test_matching_main_window_activates_once_then_stops_at_controls(self):
        self.read.side_effect = lambda: {"controls": ["ready"]} if self.active == 123 else {}
        state = self.wait()
        self.assertEqual(state["controls"], ["ready"])
        self.focus.assert_called_once_with(123)
        self.enumerate.assert_called_once()
        self.assertEqual(len(self.actions), 1)
        self.assertEqual(self.actions[0]["action"], "startup_window_activation")
        self.assertEqual(self.actions[0]["pid"], self.process.pid)
        self.assertEqual(self.actions[0]["foreground_before"], 111)
        self.assertEqual(self.actions[0]["foreground_after"], 123)
        self.assertTrue(self.actions[0]["foreground_switched"])

    def test_matching_test_title_is_eligible(self):
        self.candidate["title"] = "AkUGC Copilot OS Mouse Verification"
        self.read.side_effect = lambda: {"controls": ["ready"]} if self.active == 123 else {}
        self.wait()
        self.focus.assert_called_once_with(123)

    def test_controls_appearing_during_enumeration_prevent_activation(self):
        self.read.side_effect = [{}, {"controls": ["ready"]}]
        self.assertEqual(self.wait()["controls"], ["ready"])
        self.focus.assert_not_called()
        self.assertEqual(self.actions, [])

    def test_controls_already_present_skip_enumeration(self):
        self.read.return_value = {"controls": ["ready"]}
        self.wait()
        self.enumerate.assert_not_called()
        self.focus.assert_not_called()

    def test_actual_owner_rechecked_immediately_before_activation(self):
        self.owner.side_effect = [self.process.pid, 5555]
        self.read.side_effect = [{}, {}, {"controls": ["ready"]}]
        self.wait()
        self.focus.assert_not_called()
        self.assertEqual(self.actions, [])

    def test_startup_does_not_repeatedly_steal_focus_when_activation_denied(self):
        self.focus.side_effect = None
        self.focus.return_value = 0
        with self.assertRaises(TimeoutError):
            self.wait()
        self.focus.assert_called_once_with(123)
        self.assertEqual(len(self.actions), 1)
        self.assertFalse(self.actions[0]["activation_accepted"])
        self.assertFalse(self.actions[0]["foreground_switched"])
        self.assertEqual(self.clock, 100)

    def test_delayed_actual_foreground_switch_is_recorded_once(self):
        self.focus.side_effect = None
        self.focus.return_value = 1
        original_sleep = self.sleep

        def delayed_sleep(seconds):
            original_sleep(seconds)
            if self.clock >= 0.5:
                self.active = 123

        self.sleep = delayed_sleep
        self.read.side_effect = lambda: {"controls": ["ready"]} if self.clock >= 1 else {}
        self.wait()
        self.focus.assert_called_once_with(123)
        self.assertEqual([a["action"] for a in self.actions],
                         ["startup_window_activation", "startup_foreground_switch_observed"])

    def test_process_exit_aborts_before_enum_or_activation(self):
        self.process.poll.return_value = 1
        with self.assertRaises(RuntimeError):
            self.wait()
        self.enumerate.assert_not_called()
        self.focus.assert_not_called()

    def test_native_enum_selects_only_actual_owned_large_editor_window(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "Saved", prefix="creator-slider-unit-") as temporary:
            output = Path(temporary)
            process = Mock(pid=9876)
            process.poll.return_value = None
            active = [999]
            windows = {101: (444, True, "AkUGCPlatform - other process", 1600, 900),
                       102: (9876, False, "AkUGCPlatform - hidden", 1600, 900),
                       103: (9876, True, "AkUGCPlatform - small", 400, 300),
                       104: (9876, True, "Unrelated title", 1600, 900),
                       105: (9876, True, "AkUGCPlatform - Unreal Editor", 1600, 900)}
            user = Mock()
            user.FindWindowW.return_value = 106
            user.GetForegroundWindow.side_effect = lambda: active[0]
            user.GetWindowThreadProcessId.side_effect = lambda handle, pid: setattr(
                pid._obj, "value", windows[handle][0] if handle in windows else (9876 if handle == 106 else 444))
            user.IsWindowVisible.side_effect = lambda handle: windows[handle][1]

            def enum(callback, _):
                for handle in windows:
                    callback(handle, 0)
                return 1

            def rect(handle, pointer):
                width, height = windows[handle][3:] if handle in windows else (1600, 900)
                pointer._obj.left, pointer._obj.top = 0, 0
                pointer._obj.right, pointer._obj.bottom = width, height
                return 1

            def title(handle, text, _):
                text.value = windows[handle][2]
                return len(text.value)

            def focus(handle):
                active[0] = handle
                if handle == 105:
                    driver.write_json(output / "ui-state.json", {
                        "controls": [{"type": "SButton", "label": "新建默认可玩塔防", "enabled": True,
                                      "x": 200, "y": 200, "width": 100, "height": 30}],
                        "window_x": 0, "window_y": 0, "window_width": 1600, "window_height": 900})
                return 1

            user.EnumWindows.side_effect = enum
            user.GetWindowRect.side_effect = rect
            user.GetWindowTextW.side_effect = title
            user.SetForegroundWindow.side_effect = focus
            user.SetCursorPos.side_effect = RuntimeError("test stop before any input")
            with patch.object(ctypes, "WinDLL", return_value=user, create=True):
                with self.assertRaisesRegex(RuntimeError, "test stop"):
                    driver.drive_mouse_ui(process, output, "")
            self.assertEqual([call.args[0] for call in user.SetForegroundWindow.call_args_list], [105, 106])
            user.EnumWindows.assert_called_once()
            user.mouse_event.assert_not_called()
            user.keybd_event.assert_not_called()
            user.SendInput.assert_not_called()
            actions = driver.read_json(output / "os-input-actions.json")
            self.assertEqual(actions[0]["action"], "startup_window_activation")
            self.assertEqual(actions[0]["window"], 105)
            self.assertTrue(actions[0]["foreground_switched"])
            self.assertEqual(actions[1]["action"], "initial_focus_verified")


class RelativeSliderMotionTests(unittest.TestCase):
    def setUp(self):
        self.clock = 0.0
        self.actions = []

    def sleep(self, seconds):
        self.clock += seconds

    def attempt(self, width, send, read):
        return driver.attempt_relative_slider_drag(width, send, read, self.actions,
                                                   clock=lambda: self.clock, sleep=self.sleep)

    def test_wide_integer_sliders_cross_rounding_threshold_with_bounded_motion(self):
        # Model native min=1/max=3 SmallStep=.1 with ShiftMultiplier=10; not UE evidence.
        small_step, shift_multiplier = 0.1, 10
        for width in (1212, 1214.4, 1500, 1800, 2100):
            with self.subTest(width=width):
                self.clock, self.actions = 0.0, []
                moves = []
                state = {"slider_evidence": {"captured_waves": 3}}
                internal = [3.0]

                def send(dx):
                    moves.append(dx)
                    if len(moves) > 1:  # First event can only establish drag/capture.
                        internal[0] += 2 * dx * small_step * shift_multiplier / width
                        state["slider_evidence"]["captured_waves"] = int(internal[0] + 0.5)

                steps = self.attempt(width, send, lambda: copy.deepcopy(state))
                self.assertEqual(state["slider_evidence"]["captured_waves"], 2)
                self.assertGreater(-sum(moves[1:]) * small_step * shift_multiplier, width / 4)
                self.assertLessEqual(steps, 80)
                self.assertLess(self.clock, 7)
                self.assertTrue(all(-48 <= dx <= -12 for dx in moves))
                self.assertEqual(-moves[0], max(12, min(48, round(width / 48))))
                self.assertFalse(driver.validate_slider_evidence(state["slider_evidence"]))

    def test_native_small_step_without_shift_cannot_cross_half_integer_in_80_moves(self):
        for width in (1212, 1214.4, 1500, 1800, 2100):
            with self.subTest(width=width):
                self.clock, self.actions = 0.0, []
                internal, count = [3.0], [0]

                def send(dx):
                    count[0] += 1
                    if count[0] > 1:
                        internal[0] += 2 * dx * 0.1 / width

                def read():
                    return {"slider_evidence": {"captured_waves": int(internal[0] + 0.5)}}

                self.assertEqual(self.attempt(width, send, read), 80)
                self.assertEqual(read()["slider_evidence"]["captured_waves"], 3)
                self.assertFalse(driver.validate_slider_evidence(read()["slider_evidence"]))

    def test_captured_two_stops_immediately_without_extra_move_or_sleep(self):
        moves = []

        def send(dx):
            moves.append(dx)

        state = lambda: {"slider_evidence": {"captured_waves": 2 if len(moves) >= 3 else 3}}
        steps = self.attempt(2100, send, state)
        self.assertEqual(steps, 3)
        self.assertEqual(len(moves), 3)
        self.assertAlmostEqual(self.clock, 0.16)

    def test_new_snapshot_after_event_routing_stops_before_next_step(self):
        send = Mock()
        read = Mock(side_effect=[{"slider_evidence": {"captured_waves": 3}},
                                 {"slider_evidence": {"captured_waves": 2}}])
        self.assertEqual(self.attempt(1212, send, read), 1)
        send.assert_called_once()
        self.assertAlmostEqual(self.clock, 0.08)

    def test_no_delta_stops_at_80_steps_and_does_not_certify_success(self):
        send = Mock()
        state = {"slider_evidence": dict(VALID_EVIDENCE, captured_waves=3, released_waves=3,
                                        nonzero_delta_events=0, high_precision_delta_events=0)}
        self.assertEqual(self.attempt(2100, send, lambda: copy.deepcopy(state)), 80)
        self.assertEqual(send.call_count, 80)
        self.assertAlmostEqual(self.clock, 6.4)
        self.assertFalse(driver.validate_slider_evidence(state["slider_evidence"]))
        self.assertTrue(all(not a["physical_input_provenance_verified"] for a in self.actions))

    def test_slow_event_routing_respects_motion_deadline_before_80_steps(self):
        send = Mock()

        def read():
            self.clock += 0.22
            return {}

        steps = self.attempt(1500, send, read)
        self.assertLess(steps, 80)
        self.assertLess(self.clock, 7.3)  # At most the current read can finish beyond the deadline.
        release_timeout = min(5, max(0, 12 - self.clock))
        self.assertLessEqual(self.clock + release_timeout, 12)

    def test_step_is_clamped_and_width_is_checked(self):
        for width, expected in ((2, 12), (576, 12), (1212, 25), (2100, 44), (10000, 48)):
            self.actions = []
            send = Mock()
            self.attempt(width, send, lambda: {"slider_evidence": {"captured_waves": 2}})
            send.assert_called_once_with(-expected)
        for width in (0, -1, True, None, "2100", float("nan"), float("inf")):
            with self.subTest(width=width), self.assertRaises(RuntimeError):
                self.attempt(width, Mock(), lambda: {})


if __name__ == "__main__":
    unittest.main()
