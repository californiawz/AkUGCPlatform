import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import uuid
import ssl
from datetime import datetime, timedelta, timezone
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from unreal_automation_verdict import (
    AutomationVerdictError,
    require_automation_success,
)


def slider_evidence_diagnostics(evidence):
    if not isinstance(evidence, dict):
        return ["slider_evidence missing or not an object"]
    failures = []
    for name, expected in (("target", "wave_count"), ("target_verified", True),
                           ("released", True), ("keyboard_value_input", False)):
        if type(evidence.get(name)) is not type(expected) or evidence.get(name) != expected:
            failures.append(name + " did not match required value")
    capture = evidence.get("capture_type")
    if not isinstance(capture, str) or not capture.startswith("SSpinBox"):
        failures.append("capture_type is not SSpinBox")
    for name, expected in (("start_waves", 3), ("captured_waves", 2), ("released_waves", 2)):
        value = evidence.get(name)
        if type(value) not in (int, float) or value != expected:
            failures.append(name + " did not match required value")
    for name in ("nonzero_delta_events", "high_precision_delta_events"):
        value = evidence.get(name)
        if type(value) not in (int, float) or not 0 < value < float("inf"):
            failures.append(name + " must be positive")
    return failures


def validate_slider_evidence(evidence):
    """Validate observed Slate events, not their physical HID/WM_INPUT provenance."""
    return not slider_evidence_diagnostics(evidence)


def read_json(path):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, (dict, list)) else {}
    except (FileNotFoundError, PermissionError, json.JSONDecodeError):
        return {}


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    deadline = time.monotonic() + 3
    while True:
        try:
            temporary.replace(path)
            return
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


class ActionJournal(list):
    def __init__(self, path):
        super().__init__()
        self.path = path
        self.flush()

    def append(self, action):
        super().append(action)
        self.flush()

    def flush(self):
        write_json(self.path, self)


def fresh_control_hit(state, candidate, label, require_spinbox=False, allow_cursor_within_bounds=False):
    """The hover path and cursor must belong to the same fresh geometry snapshot."""
    fields = ("type", "label", "x", "y", "width", "height", "enabled")
    if not any(all(c.get(k) == candidate.get(k) for k in fields) for c in state.get("controls", [])):
        return False
    if not candidate.get("enabled") or candidate.get("width", 0) <= 1 or candidate.get("height", 0) <= 1:
        return False
    x, y = state.get("cursor_x"), state.get("cursor_y")
    if type(x) not in (int, float) or type(y) not in (int, float):
        return False
    x_tolerance = max(3, candidate["width"] / 2 + 1) if allow_cursor_within_bounds else 3
    y_tolerance = max(3, candidate["height"] / 2 + 1) if allow_cursor_within_bounds else 3
    if abs(x - candidate["x"]) > x_tolerance or abs(y - candidate["y"]) > y_tolerance:
        return False
    hover = state.get("hover_path", [])
    return (any(p.startswith(candidate["type"] + ":") and label in p for p in hover)
            and (not require_spinbox or any(p.startswith("SSpinBox") for p in hover)))


def owns_input_foreground(window, process_pid, menu, get_foreground, get_owner):
    active = get_foreground()
    return (bool(window) and get_owner(window) == process_pid and get_owner(active) == process_pid
            and (menu or active == window))


def recover_initial_focus(window, process_pid, get_foreground, get_owner, set_foreground,
                          clock=time.monotonic, sleep=time.sleep):
    deadline = clock() + 5
    while True:
        if not window or get_owner(window) != process_pid:
            raise RuntimeError("Verification window does not belong to launched Editor")
        if get_foreground() == window:
            return
        if clock() >= deadline:
            raise RuntimeError("Verification window could not acquire foreground within 5 seconds")
        set_foreground(window)
        sleep(0.1)


def wait_for_startup_controls(process, read_state, enumerate_windows, get_foreground, get_owner,
                              set_foreground, actions, clock=time.monotonic, sleep=time.sleep):
    """Foreground only this run's large Editor windows before Slate controls exist."""
    deadline = clock() + 100
    attempted = set()
    pending_switches = set()
    while clock() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Editor exited before startup Slate controls appeared")
        state = read_state()
        if state.get("controls"):
            return state
        active = get_foreground()
        if active in pending_switches and get_owner(active) == process.pid:
            actions.append({"action": "startup_foreground_switch_observed", "pid": process.pid,
                            "window": active})
            pending_switches.remove(active)
        candidates = [c for c in enumerate_windows()
                      if c.get("pid") == process.pid and c.get("visible")
                      and c.get("width", 0) > 800 and c.get("height", 0) > 600
                      and ("AkUGCPlatform" in c.get("title", "")
                           or "AkUGC Copilot OS Mouse Verification" in c.get("title", ""))]
        candidates.sort(key=lambda c: c["width"] * c["height"], reverse=True)
        for candidate in candidates:
            handle = candidate["window"]
            if not handle or handle in attempted or get_owner(handle) != process.pid:
                continue
            # State may have appeared during EnumWindows. Never resume startup focus after that.
            state = read_state()
            if state.get("controls"):
                return state
            if clock() >= deadline:
                break
            active = get_foreground()
            if active != handle and len(attempted) < 5:
                attempted.add(handle)
                # Recheck HWND ownership immediately before the only activation attempt.
                if get_owner(handle) != process.pid:
                    continue
                accepted = bool(set_foreground(handle))
                after = get_foreground()
                switched = after == handle and get_owner(handle) == process.pid
                actions.append({"action": "startup_window_activation", "pid": process.pid,
                                "window": handle, "foreground_before": active, "foreground_after": after,
                                "activation_accepted": accepted, "foreground_switched": switched})
                if not switched:
                    pending_switches.add(handle)
            break
        sleep(min(0.5, max(0, deadline - clock())))
    raise TimeoutError("Startup Slate controls did not appear within 100 seconds")


def parse_arguments(argv=None, root=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--all-tests", action="store_true")
    parser.add_argument("--generated-document", type=Path)
    parser.add_argument("--mouse-ui", action="store_true")
    parser.add_argument("--menu-entry", action="store_true")
    parser.add_argument("--mouse-live-authorized", action="store_true")
    parser.add_argument("--parameter-ui", action="store_true")
    sliders = parser.add_mutually_exclusive_group()
    sliders.add_argument("--slider-ui", action="store_true")
    sliders.add_argument("--manual-slider", action="store_true")
    parser.add_argument("--acceptance-nonce")
    parser.add_argument("--library-ui", action="store_true")
    parser.add_argument("--output", type=Path, help="Fresh run directory under project Saved")
    args = parser.parse_args(argv)
    if (args.slider_ui or args.manual_slider) and not args.parameter_ui:
        parser.error("Slider modes require --parameter-ui")
    if args.manual_slider:
        if not isinstance(args.acceptance_nonce, str) or re.fullmatch(
                r"[0-9a-fA-F]{32}", args.acceptance_nonce) is None:
            parser.error("--manual-slider requires a 32-digit hexadecimal --acceptance-nonce")
        args.acceptance_nonce = args.acceptance_nonce.lower()
    elif args.acceptance_nonce is not None:
        parser.error("--acceptance-nonce is only valid with --manual-slider")
    if (args.menu_entry or args.mouse_live_authorized or args.parameter_ui or args.library_ui) and not args.mouse_ui:
        parser.error("Menu, live, parameter and library modes require --mouse-ui")
    if sum((args.menu_entry, args.mouse_live_authorized, args.parameter_ui, args.library_ui)) > 1:
        parser.error("Menu, live, parameter and library modes run separately")
    if args.mouse_live_authorized:
        import os
        if not os.environ.get("DEEPSEEK_API_KEY"):
            parser.error("Live OS input requires process key")
    if args.mouse_ui and (args.all_tests or args.generated_document):
        parser.error("--mouse-ui runs separately from headless full regression")
    root = (root or Path(__file__).resolve().parents[2]).resolve()
    if args.manual_slider:
        try:
            args.creator_build_sha256 = creator_build_sha256(root)
        except OSError:
            parser.error("Cannot hash the Creator module used by --manual-slider")
    else:
        args.creator_build_sha256 = None
    saved = (root / "Saved").resolve()
    if args.generated_document:
        args.generated_document = args.generated_document.resolve()
        if not args.all_tests or not args.generated_document.is_file() or not args.generated_document.is_relative_to(saved):
            parser.error("--generated-document requires --all-tests and an existing document under project Saved")
    if args.output:
        args.output = args.output.resolve()
        if args.output == saved or not args.output.is_relative_to(saved) or args.output.exists():
            parser.error("--output must be a fresh directory strictly under project Saved")
    return args


def verification_paths(args, root):
    run_root = args.output or root / "Saved/CopilotHttpVerification" / uuid.uuid4().hex
    user_dir = run_root / "editor-user"
    output = user_dir / "Saved/verification" if args.parameter_ui or args.library_ui else run_root
    # Reserving the fresh root fails rather than overwriting any previous verification.
    run_root.mkdir(parents=True, exist_ok=False)
    if output != run_root:
        output.mkdir(parents=True, exist_ok=False)
    return output, user_dir


def attempt_relative_slider_drag(width, send_move, read_state, actions,
                                 clock=time.monotonic, sleep=time.sleep):
    """Bounded OS motion sized to the observed slider width; not proof of success."""
    if type(width) not in (int, float) or not 1 < width < float("inf"):
        raise RuntimeError("Wave slider width must be a finite positive geometry value")
    step = max(12, min(48, round(width / 48)))
    # Leave five seconds of the caller's twelve-second gesture budget for release evidence.
    deadline = clock() + 7
    actions.append({"action": "numeric_slider_motion_plan", "width": width, "step_pixels": step,
                    "max_steps": 80, "motion_budget_seconds": 7,
                    "physical_input_provenance_verified": False})
    steps = 0
    for _ in range(80):
        if clock() >= deadline:
            break
        send_move(-step)
        steps += 1
        actions.append({"action": "relative_mouse_move_sent", "dx": -step, "dy": 0,
                        "physical_input_provenance_verified": False})
        if read_state().get("slider_evidence", {}).get("captured_waves") == 2:
            break
        remaining = deadline - clock()
        if remaining <= 0:
            break
        sleep(min(0.08, remaining))
        if read_state().get("slider_evidence", {}).get("captured_waves") == 2:
            break
    return steps


def mouse_ui_command(command, args, output, user_dir):
    command = [part for part in command if part != "-nullrhi" and not part.startswith("-ExecCmds=")]
    command[0] = str(args.engine / "Engine/Binaries/Win64/UnrealEditor.exe")
    command += ["-ExecCmds=Automation RunTests AkUGC.Editor.Copilot.OSMouseInteraction", f"-CopilotMouseOutput={output}"]
    if args.menu_entry:
        command += ["-CopilotMenuEntry"]
    if args.mouse_live_authorized:
        command += ["-CopilotMouseLive", "-CopilotLiveAuthorized"]
    if args.parameter_ui:
        command += ["-CreatorParameterMouse"]
    if args.slider_ui or args.manual_slider:
        command += ["-CreatorSliderMouse"]
    if args.manual_slider:
        command += [
            "-CreatorManualSlider",
            f"-CreatorAcceptanceNonce={args.acceptance_nonce}",
            f"-CreatorBuildSha256={args.creator_build_sha256}",
        ]
    if args.library_ui:
        command += ["-CreatorLibraryMouse"]
    if args.parameter_ui or args.library_ui:
        command = [part for part in command if part != "-unattended"]
        command += ["-UnattendedInput", f"-UserDir={user_dir}"]
    return command


def creator_build_sha256(root=None):
    root = (root or Path(__file__).resolve().parents[2]).resolve()
    path = root / "Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor-AkUGCEditor.dll"
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def slider_result_details(output, args, completed=False):
    state = read_json(output / "ui-state.json")
    latest = state.get("slider_evidence") if isinstance(state, dict) else None
    latched = read_json(output / "slider-evidence.json")
    evidence = latched if validate_slider_evidence(latched) else latest
    requested = args.slider_ui or args.manual_slider
    actions = read_json(output / "os-input-actions.json")
    shift_used = args.slider_ui and isinstance(actions, list) and any(
        a.get("action") == "modifier_down" and a.get("modifier") == "LeftShift"
        and a.get("target") == "wave_count" for a in actions if isinstance(a, dict))
    input_mode = ("native_shift_accelerated_os_input" if args.slider_ui else
                  "awaited_physical_mouse" if args.manual_slider else "not_requested")
    result = {
        "numeric_slider_verified": bool(
            requested and completed and validate_slider_evidence(evidence)),
        "manual_slider": args.manual_slider,
        "human_ui_validated": False,
        "slider_input_mode": input_mode,
        "native_shift_acceleration_used": bool(shift_used),
        "physical_input_provenance_verified": False,
        "unmodified_physical_slider_verified": False,
        "slider_evidence": evidence,
        "slider_diagnostics": slider_evidence_diagnostics(evidence) if requested else [],
        "slider_event_source": "observed Slate events; physical HID/WM_INPUT origin not certified",
        "automated_slider_motion": bool(args.slider_ui),
    }
    if args.manual_slider:
        current_build_hash = creator_build_sha256()
        build_unchanged = current_build_hash == args.creator_build_sha256
        result["numeric_slider_verified"] = (
            result["numeric_slider_verified"] and build_unchanged)
        result.update({
            "acceptance_nonce": args.acceptance_nonce,
            "creator_build_sha256": args.creator_build_sha256,
            "creator_build_unchanged_during_run": build_unchanged,
        })
    return result


def drive_mouse_ui(process, output, base, menu=False, live=False, parameters=False, slider=False,
                   library=False, manual_slider=False):
    actions = ActionJournal(output / "os-input-actions.json")
    try:
        return _drive_mouse_ui(process, output, base, menu, live, parameters, slider, library,
                               manual_slider, actions)
    finally:
        actions.flush()


def _drive_mouse_ui(process, output, base, menu, live, parameters, slider, library, manual_slider, actions):
    import ctypes
    from ctypes import wintypes

    user = ctypes.WinDLL("user32", use_last_error=True)
    user.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    user.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetForegroundWindow.restype = wintypes.HWND
    user.SetForegroundWindow.argtypes = [wintypes.HWND]
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user.FindWindowW.restype = wintypes.HWND
    title = "AkUGC Copilot OS Mouse Verification"
    window = None

    class Mouse(ctypes.Structure):
        _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("data", wintypes.DWORD),
                    ("flags", wintypes.DWORD), ("time", wintypes.DWORD), ("extra", ctypes.c_size_t)]

    class Keyboard(ctypes.Structure):
        _fields_ = [("vk", wintypes.WORD), ("scan", wintypes.WORD), ("flags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("extra", ctypes.c_size_t)]

    class Union(ctypes.Union):
        _fields_ = [("mouse", Mouse), ("keyboard", Keyboard)]

    class Input(ctypes.Structure):
        _fields_ = [("type", wintypes.DWORD), ("value", Union)]

    user.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]

    def read(name="ui-state.json"):
        path = output / name
        try:
            before = path.stat().st_mtime_ns
            state = read_json(path)
            if not isinstance(state, dict) or path.stat().st_mtime_ns != before:
                return {}
            state["_snapshot_version"] = before
            return state
        except (FileNotFoundError, PermissionError):
            return {}

    def owner(handle):
        pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(handle, ctypes.byref(pid))
        return pid.value

    def foreground():
        if not owns_input_foreground(
                window, process.pid, menu, user.GetForegroundWindow, owner):
            raise RuntimeError("Verification window lost foreground; stopped without sending input elsewhere")

    def wait(check, timeout=30):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if process.poll() is not None:
                raise RuntimeError("Editor exited before OS UI validation finished")
            if window is not None:
                foreground()
            state = read()
            if check(state):
                return state
            time.sleep(min(0.1, max(0, end - time.monotonic())))
        raise TimeoutError("OS UI expected state not reached")

    def startup_windows():
        candidates = []

        def collect(handle, _):
            pid = owner(handle)
            if pid != process.pid or not user.IsWindowVisible(handle):
                return True
            rect = wintypes.RECT()
            if not user.GetWindowRect(handle, ctypes.byref(rect)):
                return True
            width, height = rect.right - rect.left, rect.bottom - rect.top
            if width <= 800 or height <= 600:
                return True
            text = ctypes.create_unicode_buffer(512)
            user.GetWindowTextW(handle, text, len(text))
            candidates.append({"window": handle, "pid": pid, "visible": True,
                               "width": width, "height": height, "title": text.value})
            return True

        if not user.EnumWindows(callback_type(collect), 0):
            raise ctypes.WinError(ctypes.get_last_error())
        return candidates

    state = wait_for_startup_controls(process, read, startup_windows, user.GetForegroundWindow,
                                      owner, user.SetForegroundWindow, actions,
                                      clock=time.monotonic, sleep=time.sleep)
    if menu or manual_slider:
        title = state["window_title"]
    window = user.FindWindowW(None, title)
    recover_initial_focus(window, process.pid, user.GetForegroundWindow, owner, user.SetForegroundWindow)
    actions.append({"action": "initial_focus_verified", "pid": process.pid})

    def position_cursor(x, y, state=None):
        foreground()
        state = state or wait(lambda s: bool(s.get("controls")), 5)
        rect = wintypes.RECT()
        if not user.GetWindowRect(window, ctypes.byref(rect)):
            raise ctypes.WinError(ctypes.get_last_error())
        physical_x = rect.left + (x - state["window_x"]) * (rect.right - rect.left) / state["window_width"]
        physical_y = rect.top + (y - state["window_y"]) * (rect.bottom - rect.top) / state["window_height"]
        foreground()
        if not user.SetCursorPos(int(physical_x), int(physical_y)):
            raise ctypes.WinError(ctypes.get_last_error())
        foreground()

    def mouse(flags, dx=0, dy=0, data=0):
        foreground()
        user.mouse_event(flags, dx, dy, data, 0)

    def release_mouse():
        if owns_input_foreground(
                window, process.pid, menu, user.GetForegroundWindow, owner):
            user.mouse_event(0x0004, 0, 0, 0, 0)
        else:
            actions.append({"action": "release_skipped_after_focus_loss"})

    def control(label, kind="SButton", scroll=False, require_spinbox=False, move=True,
                allow_cursor_within_bounds=False):
        for _ in range(24 if scroll else 3):
            state = wait(lambda s: bool(s.get("controls")), 5)
            candidates = [c for c in state.get("controls", []) if (c["type"] == kind or kind == "SNumericEntryBox" and c["type"].startswith(kind)) and label in c["label"]
                          and c["width"] > 1 and c["height"] > 1
                          and state["window_x"] + 5 < c["x"] < state["window_x"] + state["window_width"] - 5
                          and state["window_y"] + (0 if menu else 60) < c["y"] < state["window_y"] + state["window_height"] - (0 if menu else 90)]
            if candidates:
                candidate = candidates[0]
                if not candidate["enabled"] or not move:
                    return candidate
                version = state["_snapshot_version"]
                position_cursor(candidate["x"], candidate["y"], state)
                try:
                    wait(lambda s: s.get("_snapshot_version", 0) > version
                         and fresh_control_hit(s, candidate, label, require_spinbox,
                                               allow_cursor_within_bounds), 1)
                    return candidate
                except TimeoutError:
                    pass
            if scroll:
                position_cursor(state["window_x"] + state["window_width"] * 0.8,
                                state["window_y"] + state["window_height"] * 0.75, state)
                mouse(0x0800, data=ctypes.c_uint32(-360).value)
                time.sleep(0.3)
        raise RuntimeError(f"Visible UI control not found with a fresh Slate hit: {label}")

    def click(label, kind="SButton", scroll=False, allow_cursor_within_bounds=False):
        c = control(label, kind, scroll,
                    allow_cursor_within_bounds=allow_cursor_within_bounds)
        if not c["enabled"]:
            raise RuntimeError(f"UI control unexpectedly disabled: {label}")
        if not fresh_control_hit(read(), c, label,
                                 allow_cursor_within_bounds=allow_cursor_within_bounds):
            raise RuntimeError("UI geometry or cursor changed before mouse down")
        actions.append({"action": "mouse_click_started", "label": label, "x": c["x"], "y": c["y"],
                        "slate_hit_verified": True})
        mouse(0x0002)
        try:
            foreground()
        finally:
            release_mouse()
        time.sleep(0.35)
        actions.append({"action": "mouse_click", "label": label, "x": c["x"], "y": c["y"],
                        "slate_hit_verified": True})

    def fill(label, text, kind="SEditableTextBox"):
        if (slider or manual_slider) and kind == "SNumericEntryBox" and "波数" in label:
            raise RuntimeError("Slider mode must never use keyboard value input for wave count")
        click(label, kind, not parameters)
        held_keys = set()

        def key(vk, up=False):
            foreground()
            user.keybd_event(vk, 0, 2 if up else 0, 0)
            held_keys.discard(vk) if up else held_keys.add(vk)

        actions.append({"action": "keyboard_input_started", "label": label, "characters": len(text)})
        try:
            key(0x11)
            key(0x41)
            key(0x41, True)
            key(0x11, True)
            key(0x08)
            key(0x08, True)
            for ch in text:
                foreground()
                events = (Input * 2)()
                events[0].type = events[1].type = 1
                events[0].value.keyboard = Keyboard(0, ord(ch), 4, 0, 0)
                events[1].value.keyboard = Keyboard(0, ord(ch), 6, 0, 0)
                if user.SendInput(2, events, ctypes.sizeof(Input)) != 2:
                    raise ctypes.WinError(ctypes.get_last_error())
            if kind == "SNumericEntryBox":
                key(0x0D)
                key(0x0D, True)
        finally:
            if user.GetForegroundWindow() == window and owner(window) == process.pid:
                for vk in held_keys:
                    user.keybd_event(vk, 0, 2, 0)
        time.sleep(0.4)
        actions.append({"action": "keyboard_input", "label": label, "characters": len(text)})

    seq = 0

    def checkpoint(tag):
        nonlocal seq
        seq += 1
        temporary = output / "ui-command.tmp"
        temporary.write_text(json.dumps({"seq": seq, "tag": tag}), encoding="utf-8")
        deadline = time.monotonic() + 3
        while True:
            try:
                temporary.replace(output / "ui-command.json")
                break
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        if tag != "finish":
            end = time.monotonic() + 10
            while time.monotonic() < end:
                ack = read("ui-ack.json")
                if ack.get("ack") == seq:
                    if not ack.get("checkpoint_passed"):
                        raise RuntimeError(f"UE UI checkpoint failed: {tag}")
                    return
                time.sleep(0.1)
            raise TimeoutError(f"UI checkpoint not acknowledged: {tag}")

    if menu:
        state = read()
        labels = [c["label"] for c in state["controls"] if c["type"] == "STextBlock"]
        window_label = next((t for t in labels if t in ("窗口", "窗口(W)", "Window")), None)
        if not window_label:
            raise RuntimeError("Editor Window menu label not found")
        click(window_label, "STextBlock", allow_cursor_within_bounds=True)
        wait(lambda s: any(c["label"] == "AkUGC Creator Studio" for c in s.get("controls", [])))
        click("AkUGC Creator Studio", "STextBlock", allow_cursor_within_bounds=True)
        wait(lambda s: s.get("creator_tab_open"))
        checkpoint("menu")
        checkpoint("finish")
        (output / "os-input-actions.json").write_text(json.dumps(actions, ensure_ascii=False, indent=2), encoding="utf-8")
        return

    if library or parameters:
        state = read()
        for name in (("library_directory", "project_path") if library else ("project_path",)):
            if not state.get(name) or not Path(state[name]).resolve().is_relative_to(output.parent.resolve()):
                raise RuntimeError("Creator test path escaped isolated Saved root")
    click("新建默认可玩塔防")
    wait(lambda s: s.get("project_open"))
    checkpoint("new")
    if library:
        import hashlib
        click("Save")
        checkpoint("library-saved")
        click("我已通过独立渠道", "SCheckBox", scroll=True)
        wait(lambda s: s.get("trust_confirmed"))
        click("一键签名发布当前项目", scroll=True)
        published = wait(lambda s: s.get("library_count") == 1)
        checkpoint("published")
        release, public_key = published["release_id"], published["publisher_public_key"]
        share_dir = output.parent / "UGC/Share"
        original = share_dir / (release + ".akugc")
        original_digest = hashlib.sha256(original.read_bytes()).hexdigest()
        click("导出分享:" + release, scroll=True)
        wait(lambda s: len(list(share_dir.glob("*.akugc"))) == 2)
        checkpoint("exported")
        exported = next(p for p in share_dir.glob("*.akugc") if p != original)
        exported_digest = hashlib.sha256(exported.read_bytes()).hexdigest()
        click("删除本地记录:" + release, scroll=True)
        wait(lambda s: s.get("delete_pending"))
        disabled_rows = [c for c in read().get("controls", []) if c["type"] == "SButton"
                         and c["label"] in ("导出分享:" + release, "删除本地记录:" + release)]
        if len(disabled_rows) != 2 or any(c["enabled"] for c in disabled_rows):
            raise RuntimeError("Work row remained enabled during delete confirmation")
        checkpoint("delete-pending")
        click("取消删除", scroll=True)
        wait(lambda s: not s.get("delete_pending"))
        if not control("导出分享:" + release, scroll=True)["enabled"]:
            raise RuntimeError("Work row did not recover after cancelling deletion")
        checkpoint("delete-cancelled")
        click("删除本地记录:" + release, scroll=True)
        wait(lambda s: s.get("delete_pending"))
        click("确认删除记录", scroll=True)
        wait(lambda s: s.get("library_count") == 0 and not s.get("delete_pending"))
        checkpoint("deleted")
        fill("接收的 .akugc", str(exported))
        fill("独立核对的 Ed25519", public_key)
        imports = [c for c in read().get("controls", []) if c["type"] == "SButton" and c["label"] == "验签并导入作品库"]
        if len(imports) != 1 or imports[0]["enabled"]:
            raise RuntimeError("Import enabled without explicit trust")
        checkpoint("untrusted")
        wrong_key = ("0" if public_key[0] != "0" else "1") + public_key[1:]
        fill("独立核对的 Ed25519", wrong_key)
        click("我已通过独立渠道", "SCheckBox", scroll=True)
        wait(lambda s: s.get("trust_confirmed"))
        click("验签并导入作品库", scroll=True)
        wait(lambda s: not s.get("trust_confirmed") and s.get("library_count") == 0)
        checkpoint("wrong-key")
        fill("独立核对的 Ed25519", public_key)
        for tag in ("imported", "import-idempotent"):
            click("我已通过独立渠道", "SCheckBox", scroll=True)
            wait(lambda s: s.get("trust_confirmed"))
            click("验签并导入作品库", scroll=True)
            wait(lambda s: not s.get("trust_confirmed") and s.get("library_count") == 1)
            checkpoint(tag)
        if hashlib.sha256(original.read_bytes()).hexdigest() != original_digest or hashlib.sha256(exported.read_bytes()).hexdigest() != exported_digest:
            raise RuntimeError("Deletion/import changed retained share bytes")
        actions.append({"action": "retained_share_sha256_verified", "files": 2,
                        "publisher_key_source": "separate publication state, not imported envelope",
                        "human_trust_channel_verified": False})
        checkpoint("finish")
        (output / "os-input-actions.json").write_text(json.dumps(actions, ensure_ascii=False, indent=2), encoding="utf-8")
        return
    if parameters:
        wait(lambda s: s.get("draft_waves") == 3)
        if slider or manual_slider:
            label = "波数（1–3，默认 3）"
            before = read()
            if (type(before.get("document_revision")) not in (int, float)
                    or type(before.get("can_undo")) is not bool or type(before.get("can_redo")) is not bool):
                raise RuntimeError("Slider baseline document revision or history evidence is missing")
            c = control(label, "SNumericEntryBox", require_spinbox=True, move=not manual_slider)
            if not c["enabled"]:
                raise RuntimeError("Wave slider is disabled")
            if manual_slider:
                instructions = "请在本验证窗口用物理鼠标按住波数框，向左拖动默认 3 至 2 后松开；不要键盘输入。"
                write_json(output / "slider-ready.json", {
                    "phase": "awaiting_physical_mouse", "instructions": instructions,
                    "pid": process.pid, "target": "wave_count", "target_geometry": c,
                    "acceptance_nonce": before.get("acceptance_nonce"),
                    "creator_build_sha256": before.get("creator_build_sha256"),
                    "window": {k: before[k] for k in ("window_x", "window_y", "window_width", "window_height")},
                    "timeout_seconds": 240})
                actions.append({
                    "action": "awaiting_physical_mouse",
                    "target": "wave_count",
                    "acceptance_nonce": before.get("acceptance_nonce"),
                    "creator_build_sha256": before.get("creator_build_sha256"),
                })
                print(instructions, flush=True)
                captured = wait(lambda s: validate_slider_evidence(s.get("slider_evidence"))
                                and s.get("draft_waves") == 2 and not s.get("mouse_captured"), 240)
            else:
                if not fresh_control_hit(read(), c, label, require_spinbox=True):
                    raise RuntimeError("Wave slider geometry or cursor changed before mouse down")
                actions.append({"action": "numeric_slider_drag_started", "target": "wave_count",
                                "slate_hit_verified": True, "start_waves": 3})
                gesture_deadline = time.monotonic() + 12
                shift_held = False
                try:
                    foreground()
                    user.keybd_event(0xA0, 0x2A, 0, 0)  # LeftShift enables the native slider multiplier.
                    shift_held = True
                    actions.append({"action": "modifier_down", "modifier": "LeftShift",
                                    "target": "wave_count", "purpose": "native_slider_acceleration",
                                    "physical_input_provenance_verified": False})
                    mouse(0x0002)
                    try:
                        attempt_relative_slider_drag(c["width"], lambda dx: mouse(0x0001, dx=dx), read,
                                                     actions, clock=time.monotonic, sleep=time.sleep)
                    finally:
                        # Mouse release must reach the slider while Shift is still held.
                        release_mouse()
                finally:
                    if shift_held:
                        if user.GetForegroundWindow() == window and owner(window) == process.pid:
                            user.keybd_event(0xA0, 0x2A, 2, 0)
                            actions.append({"action": "modifier_up", "modifier": "LeftShift",
                                            "target": "wave_count"})
                        else:
                            actions.append({"action": "modifier_release_skipped_after_focus_loss",
                                            "modifier": "LeftShift", "target": "wave_count"})
                captured = wait(lambda s: validate_slider_evidence(s.get("slider_evidence"))
                                and s.get("draft_waves") == 2 and not s.get("mouse_captured"),
                                min(5, max(0, gesture_deadline - time.monotonic())))
            if (captured.get("document_revision") != before.get("document_revision")
                    or captured.get("can_undo") != before.get("can_undo")
                    or captured.get("can_redo") != before.get("can_redo")):
                raise RuntimeError("Slider changed document revision or history before Apply")
            write_json(output / "slider-evidence.json", captured["slider_evidence"])
            actions.append({"action": "numeric_slider_evidence_validated", "target": "wave_count",
                            "evidence_source": "Slate IInputProcessor observation",
                            "physical_input_provenance_verified": False})
            checkpoint("slider")
        else:
            fill("波数", "2", "SNumericEntryBox")
        fill("基地生命", "1700", "SNumericEntryBox")
        fill("初始资源", "425", "SNumericEntryBox")
        wait(lambda s: s.get("draft_waves") == 2 and s.get("draft_resources") == 425 and s.get("draft_base_health") == 1700)
        checkpoint("draft")
        click("Save")
        wait(lambda s: s.get("draft_waves") == 2)
        checkpoint("saved-draft")
        click("应用参数", scroll=True)
        wait(lambda s: s.get("can_undo") and s.get("draft_waves") == 2)
        checkpoint("parameters-applied")
        click("Undo")
        wait(lambda s: not s.get("can_undo") and s.get("draft_waves") == 3)
        checkpoint("parameters-undo")
        click("Redo")
        wait(lambda s: s.get("can_undo") and s.get("draft_resources") == 425 and s.get("draft_waves") == 2)
        checkpoint("parameters-redo")
        click("Save")
        wait(lambda s: s.get("draft_waves") == 2)
        checkpoint("saved-applied")
        click("Load")
        wait(lambda s: not s.get("can_undo") and not s.get("can_redo") and s.get("draft_waves") == 2)
        checkpoint("reloaded")
        fill("初始资源", "999", "SNumericEntryBox")
        wait(lambda s: s.get("draft_resources") == 999 and s.get("draft_waves") == 2)
        click("重新读取项目 / 放弃草稿", scroll=True)
        wait(lambda s: s.get("draft_resources") == 425 and s.get("draft_waves") == 2)
        checkpoint("discarded")
        checkpoint("finish")
        return
    click("发布与本地作品库", scroll=True)
    time.sleep(0.5)
    click("AI Copilot", scroll=True)
    time.sleep(0.5)
    fill("完整HTTPS", "https://api.deepseek.com/chat/completions" if live else base + "/ok")
    fill("模型名称", "deepseek-flash" if live else "protocol-fixture-not-ai")
    if live:
        import os
        key = os.environ.pop("DEEPSEEK_API_KEY")
        try:
            fill("API Key（", key)
        finally:
            del key
    fill("自然语言需求", "设置两波敌人，每波8个，初始资源300，其余参数保持原值，不添加Logic。" if live else "Set two waves, 8 enemies each, initial resources 300.")
    click("发送需求并生成预览", scroll=True)
    wait(lambda s: s.get("preview_validated") and not s.get("pending"), 70 if live else 30)
    if live:
        fill("API Key（", "")
        wait(lambda s: s.get("api_key_empty"))
    checkpoint("preview")
    click("应用已验证提案", scroll=True)
    wait(lambda s: s.get("can_undo") and not s.get("preview_validated"))
    checkpoint("applied")
    click("Undo")
    wait(lambda s: not s.get("can_undo") and s.get("can_redo"))
    checkpoint("undo")
    click("Redo")
    wait(lambda s: s.get("can_undo"))
    checkpoint("redo")
    if live:
        checkpoint("finish")
        (output / "os-input-actions.json").write_text(json.dumps(actions, ensure_ascii=False, indent=2), encoding="utf-8")
        return
    click("Undo")
    wait(lambda s: not s.get("can_undo"))
    fill("完整HTTPS", base + "/delayed")
    click("发送需求并生成预览", scroll=True)
    wait(lambda s: s.get("pending"), 5)
    click("取消请求 / 放弃预览", scroll=True)
    time.sleep(1.5)
    wait(lambda s: not s.get("pending") and not s.get("preview_validated"))
    checkpoint("cancel")
    fill("完整HTTPS", base + "/ui-invalid")
    click("发送需求并生成预览", scroll=True)
    wait(lambda s: not s.get("pending") and "parameters.schemaVersion" in s.get("status", ""))
    if control("应用已验证提案", scroll=True)["enabled"]:
        raise RuntimeError("Apply remained enabled after rejected proposal")
    checkpoint("rejected")
    fill("完整HTTPS", base + "/explain-ok")
    click("发送校验诊断并请求模型解释", scroll=True)
    wait(lambda s: not s.get("pending") and bool(s.get("explanation")))
    if control("应用已验证提案", scroll=True)["enabled"]:
        raise RuntimeError("Read-only explanation enabled Apply")
    checkpoint("explained")
    fill("API Key（", "fixture-not-a-real-key")
    wait(lambda s: s.get("api_key_empty") is False)
    fill("API Key（", "")
    wait(lambda s: s.get("api_key_empty"))
    checkpoint("finish")
    (output / "os-input-actions.json").write_text(json.dumps(actions, ensure_ascii=False, indent=2), encoding="utf-8")


def main(argv=None):
    args = parse_arguments(argv)
    root = Path(__file__).resolve().parents[2]
    output, user_dir = verification_paths(args, root)
    try:
        return _run_verification(args, root, output, user_dir)
    except BaseException as error:
        if args.mouse_ui:
            result = read_json(output / "result.json")
            result = result if isinstance(result, dict) else {}
            result.update(slider_result_details(output, args))
            result.update({"passed": False, "error_type": type(error).__name__,
                           "error": "Mouse UI driver stopped; inspect action journal and slider diagnostics",
                           "actions": read_json(output / "os-input-actions.json"),
                           "automated_os_input": True})
            write_json(output / "result.json", result)
            print(output / "result.json", flush=True)
        raise
    finally:
        if args.mouse_ui:
            # Keep the incrementally written journal even after startup/input exceptions.
            actions = read_json(output / "os-input-actions.json")
            write_json(output / "os-input-actions.json", actions if isinstance(actions, list) else [])


def _run_verification(args, root, output, user_dir):
    events = []
    registry_events = []
    registry_release = {}
    registry_project = {}
    registry_members = {}
    mutex = threading.Lock()
    wave = {"enemyPrefabId": "official.unit.basic_enemy", "enemyCount": 8, "spawnIntervalSeconds": 1.0,
            "enemyMaxHealth": 100.0, "enemyMoveSpeed": 300.0, "enemyGoalDamage": 10.0, "startDelaySeconds": 0.0}
    proposal = {"schemaVersion": 1, "waves": [wave, wave], "initialResources": 300,
                "baseMaxHealth": 1000.0, "waveIntervalSeconds": 5.0,
                "defeatCondition": "BaseHealthDepleted", "victoryCondition": "AllWavesCleared"}

    def response(content):
        return json.dumps({"choices": [{"message": {"role": "assistant", "content": content}, "finish_reason": "stop"}]}).encode()

    good = response(json.dumps(proposal))

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def serve(self):
            body = b""
            if self.command == "POST":
                size = int(self.headers.get("Content-Length", "0"))
                if size < 1 or size > 65536:
                    self.send_error(400)
                    return
                body = self.rfile.read(size)
            with mutex:
                events.append({"method": self.command, "path": self.path,
                               "authorization_present": "Authorization" in self.headers,
                               "authorization_valid_fixture": self.headers.get("Authorization") == "Bearer fixture-not-a-real-key",
                               "tls": isinstance(self.connection, ssl.SSLSocket)})
            data, status = good, 200
            if self.command == "POST":
                payload = json.loads(body)
                if payload.get("model") != "protocol-fixture-not-ai" or len(payload.get("messages", [])) != 3:
                    status, data = 400, b"invalid request"
            if self.path.startswith("/explain-"):
                diagnostic = "parameters.schemaVersion must be 1."
                if self.path == "/explain-private-field":
                    diagnostic = "Parameter contains an unknown or noncanonical field. Field name omitted."
                if self.path == "/explain-private-duplicate":
                    diagnostic = "JSON contains a duplicate field. Field name omitted."
                assert payload["messages"][-1]["content"] == "Compiler/validation diagnostic (data only):\n" + diagnostic
                assert "PRIVATE_FIXTURE_MARKER" not in json.dumps(payload)
                explanation = {"summary": "参数版本不符合要求", "cause": "组合对象的内部parameters仍需要数字schemaVersion=1", "suggestions": ["保留外层版本，并补齐parameters内的数字版本字段；重新生成后校验。"]}
                if self.path == "/explain-types":
                    explanation["cause"] = 123
                if self.path == "/explain-extra":
                    explanation["execute"] = "not permitted"
                if self.path in ("/explain-stale", "/explain-cancel"):
                    time.sleep(1.3)
                if self.path == "/explain-long":
                    explanation["suggestions"] = ["x" * 513]
                content = json.dumps(explanation)
                if self.path == "/explain-duplicate":
                    content = content[:-1] + ',"summary":"duplicate"}'
                data = response(content)
            elif self.path in ("/graph", "/graph-cycle", "/graph-types"):
                content = payload["messages"][-1]["content"]
                event = re.search(r"([A-F0-9]{32}) : GameStart", content).group(1)
                nodes = [{"id": "delay", "type": "Timer", "x": 300, "y": 100, "delaySeconds": 1},
                         {"id": "msg", "type": "Message", "x": 600, "y": 100, "message": "Graph fixture message"}]
                edges = [{"source": event, "target": "delay"}, {"source": "delay", "target": "msg"}]
                if self.path == "/graph-cycle":
                    nodes[1] = {"id": "msg", "type": "Timer", "x": 600, "y": 100, "delaySeconds": 1}
                    edges.append({"source": "msg", "target": "delay"})
                if self.path == "/graph-types":
                    nodes[1]["message"] = 123
                data = response(json.dumps({"schemaVersion": 1, "parameters": proposal,
                                            "logic": {"nodes": nodes, "connections": edges}}))
            elif self.path == "/ui-invalid":
                data = response(json.dumps({"schemaVersion": 1, "parameters": {}, "logic": {"nodes": [], "connections": []}}))
            elif self.path == "/delayed":
                time.sleep(1.3)
                data = response(json.dumps(dict(proposal, initialResources=777)))
            elif self.path == "/new-result":
                time.sleep(0.6)
            elif self.path == "/timeout-delayed":
                time.sleep(3.0)
            elif self.path == "/unauthorized":
                status, data = 401, b'{"error":"fixture unauthorized"}'
            elif self.path == "/malformed":
                data = b"not-json"
            elif self.path == "/outer-duplicate-choices":
                data = b'{"choices":[],"choices":' + json.dumps(json.loads(good)["choices"]).encode() + b'}'
            elif self.path == "/outer-duplicate-content":
                data = b'{"choices":[{"message":{"content":"invalid","content":' + json.dumps(json.dumps(proposal)).encode() + b'}}]}'
            elif self.path == "/bad-choice":
                data = b'{"choices":[{"message":[]}]}'
            elif self.path == "/markdown":
                data = response("```json\n" + json.dumps(proposal) + "\n```")
            elif self.path == "/extra-field":
                invalid = dict(proposal, execute="arbitrary-script")
                data = response(json.dumps(invalid))
            elif self.path in ("/oversize", "/chunked"):
                data = b"x" * (1024 * 1024)
            elif self.path == "/boundary":
                data = good + b" " * (262144 - len(good))
            elif self.path == "/boundary-plus-one":
                data = good + b" " * (262145 - len(good))
            if self.path in ("/redirect", "/redirect-307", "/tls-redirect302", "/tls-redirect307", "/tls-redirect-downgrade"):
                self.send_response(307 if self.path.endswith("307") else 302)
                target = f"http://127.0.0.1:{server.server_port}/redirect-target" if self.path == "/tls-redirect-downgrade" else f"{'https' if self.path.startswith('/tls-') else 'http'}://localhost:{self.server.server_port}/redirect-target"
                self.send_header("Location", target)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            try:
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                if self.path == "/chunked":
                    self.send_header("Transfer-Encoding", "chunked")
                else:
                    self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                for index in range(0, len(data), 8192):
                    chunk = data[index:index + 8192]
                    if self.path == "/chunked":
                        self.wfile.write(f"{len(chunk):X}\r\n".encode() + chunk + b"\r\n")
                    else:
                        self.wfile.write(chunk)
                    self.wfile.flush()
                    if len(data) > 262144:
                        time.sleep(0.003)
                if self.path == "/chunked":
                    self.wfile.write(b"0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

        def handle(self):
            try:
                super().handle()
            except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
                pass

        do_POST = serve
        do_GET = serve

    class RegistryHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def send_json(self, status, value):
            data = json.dumps(value, separators=(",", ":")).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def release(self, status, revision):
            return {
                "releaseId": registry_release["release_id"],
                "projectId": registry_release["project_id"],
                "schemaVersion": registry_release["schema_version"],
                "ownerSubject": "creator-a",
                "submittedBySubject": "creator-a",
                "publisherPublicKey": registry_release["public_key"],
                "contentHash": registry_release["content_hash"],
                "artifactSha256": registry_release["artifact_sha256"],
                "artifactSize": registry_release["artifact_size"],
                "status": status,
                "revision": revision,
                "submittedAt": "2026-10-15T06:00:00Z",
                "updatedAt": f"2026-10-15T06:0{revision - 1}:00Z",
            }

        def build(self):
            status = registry_release["build_status"]
            value = {
                "releaseId": registry_release["release_id"],
                "projectId": registry_release["project_id"],
                "status": status,
                "attempt": 0 if status == "queued" else 1,
                "revision": registry_release["build_revision"],
                "retryable": status == "failed",
                "inputArtifactSha256": registry_release["artifact_sha256"],
                "createdAt": "2026-10-15T06:00:00Z",
                "updatedAt": "2026-10-15T06:00:02Z",
            }
            if status == "failed":
                value.update(
                    {
                        "errorCode": "validator_unavailable",
                        "startedAt": "2026-10-15T06:00:01Z",
                        "finishedAt": "2026-10-15T06:00:02Z",
                    }
                )
            elif status == "succeeded":
                value.update(
                    {
                        "outputArtifactSha256": registry_release["artifact_sha256"],
                        "outputArtifactSize": registry_release["artifact_size"],
                        "startedAt": "2026-10-15T06:00:01Z",
                        "finishedAt": "2026-10-15T06:00:02Z",
                    }
                )
            return value

        def project(self):
            revision = registry_project["revision"]
            return {
                "projectId": registry_project["project_id"],
                "displayName": registry_project["display_name"],
                "ownerSubject": "creator-a",
                "visibility": registry_project["visibility"],
                "revision": revision,
                "createdAt": "2026-10-15T05:00:00Z",
                "updatedAt": f"2026-10-15T05:0{revision - 1}:00Z",
            }

        def member(self, subject, role):
            return {
                "projectId": registry_project["project_id"],
                "subject": subject,
                "role": role,
                "addedAt": "2026-10-15T05:01:00Z",
                "updatedAt": "2026-10-15T05:02:00Z",
            }

        def serve(self):
            authorization_valid = (
                self.headers.get("Authorization")
                == "Bearer registry-creator-token"
            )
            size = int(self.headers.get("Content-Length", "0"))
            event = {
                "method": self.command,
                "path": self.path,
                "authorization_valid": authorization_valid,
                "content_type": self.headers.get("Content-Type", ""),
                "idempotency_key": self.headers.get("Idempotency-Key", ""),
                "content_length": size,
            }
            with mutex:
                registry_events.append(event)
            body = self.rfile.read(size) if size else b""
            if not authorization_valid:
                self.send_json(401, {"error": {"code": "unauthenticated"}})
                return
            project_prefix = "/v1/projects/"
            if self.command == "GET" and self.path.startswith(project_prefix):
                suffix = self.path[len(project_prefix):]
                requested_id = suffix.split("/", 1)[0]
                try:
                    requested_id = str(uuid.UUID(requested_id)).lower()
                except ValueError:
                    self.send_json(400, {"error": {"code": "invalid_input"}})
                    return
                if "project_id" not in registry_project:
                    registry_project["requested_id"] = requested_id
                    self.send_json(404, {"error": {"code": "not_found"}})
                    return
            if self.command == "POST" and self.path == "/v1/projects":
                try:
                    value = json.loads(body.decode("utf-8"))
                    project_id = str(uuid.UUID(value["projectId"])).lower()
                    display_name = value["displayName"]
                except (KeyError, TypeError, ValueError, UnicodeDecodeError, json.JSONDecodeError):
                    self.send_json(400, {"error": {"code": "invalid_input"}})
                    return
                if (
                    event["content_type"] != "application/json"
                    or event["idempotency_key"].lower()
                        != "akugc-project-" + project_id.replace("-", "")
                    or registry_project.get("requested_id") != project_id
                    or not isinstance(display_name, str)
                    or not display_name
                ):
                    self.send_json(400, {"error": {"code": "invalid_input"}})
                    return
                registry_project.clear()
                registry_project.update(
                    {
                        "project_id": project_id,
                        "display_name": display_name,
                        "visibility": "private",
                        "revision": 1,
                    }
                )
                self.send_json(
                    201,
                    {"created": True, "project": self.project()},
                )
                return
            if "project_id" in registry_project:
                project_id = registry_project["project_id"]
                project_path = f"/v1/projects/{project_id}"
                if self.command == "GET" and self.path == project_path:
                    self.send_json(
                        200,
                        {"accessRole": "owner", "project": self.project()},
                    )
                    return
                if self.command == "GET" and self.path == project_path + "/members":
                    self.send_json(
                        200,
                        {
                            "members": [self.member("creator-a", "owner")]
                            + [
                                self.member(subject, role)
                                for subject, role in sorted(registry_members.items())
                            ]
                        },
                    )
                    return
                if (
                    self.command == "GET"
                    and self.path == project_path + "/versions?limit=100"
                ):
                    versions = []
                    if registry_release:
                        versions.append(
                            self.release(
                                registry_release.get("status", "submitted"),
                                registry_release.get("revision", 1),
                            )
                        )
                    self.send_json(200, {"versions": versions})
                    return
                if (
                    self.command == "PUT"
                    and self.path == project_path + "/visibility"
                ):
                    try:
                        value = json.loads(body.decode("utf-8"))
                        visibility = value["visibility"]
                    except (
                        KeyError,
                        TypeError,
                        UnicodeDecodeError,
                        json.JSONDecodeError,
                    ):
                        self.send_json(400, {"error": {"code": "invalid_input"}})
                        return
                    if (
                        event["content_type"] != "application/json"
                        or visibility not in ("private", "public")
                    ):
                        self.send_json(400, {"error": {"code": "invalid_input"}})
                        return
                    changed = registry_project["visibility"] != visibility
                    registry_project["visibility"] = visibility
                    if changed:
                        registry_project["revision"] += 1
                    self.send_json(
                        200,
                        {"changed": changed, "project": self.project()},
                    )
                    return
                member_prefix = project_path + "/members/"
                if self.path.startswith(member_prefix):
                    subject = self.path[len(member_prefix):]
                    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}", subject):
                        self.send_json(400, {"error": {"code": "invalid_input"}})
                        return
                    if self.command == "PUT":
                        try:
                            value = json.loads(body.decode("utf-8"))
                            role = value["role"]
                        except (KeyError, TypeError, UnicodeDecodeError, json.JSONDecodeError):
                            self.send_json(400, {"error": {"code": "invalid_input"}})
                            return
                        if (
                            event["content_type"] != "application/json"
                            or role not in ("editor", "viewer")
                        ):
                            self.send_json(400, {"error": {"code": "invalid_input"}})
                            return
                        changed = registry_members.get(subject) != role
                        registry_members[subject] = role
                        if changed:
                            registry_project["revision"] += 1
                        self.send_json(
                            200,
                            {
                                "changed": changed,
                                "member": self.member(subject, role),
                                "project": self.project(),
                            },
                        )
                        return
                    if self.command == "DELETE":
                        changed = subject in registry_members
                        registry_members.pop(subject, None)
                        if changed:
                            registry_project["revision"] += 1
                        self.send_json(
                            200,
                            {"changed": changed, "project": self.project()},
                        )
                        return
            if self.command == "POST" and self.path == "/v1/releases":
                if (
                    size < 1
                    or size > 16 * 1024 * 1024
                    or event["content_type"] != "application/vnd.akugc+json"
                    or not re.fullmatch(
                        r"[A-Za-z0-9._:-]{16,128}",
                        event["idempotency_key"],
                    )
                ):
                    self.send_json(400, {"error": {"code": "invalid_input"}})
                    return
                artifact = body
                try:
                    pack = json.loads(artifact.decode("utf-8-sig"))
                    manifest = pack["manifest"]
                    signature = pack["signature"]
                    release_id = str(uuid.UUID(manifest["releaseId"])).lower()
                    project_id = str(uuid.UUID(manifest["projectId"])).lower()
                except (KeyError, TypeError, ValueError, UnicodeDecodeError, json.JSONDecodeError):
                    self.send_json(400, {"error": {"code": "invalid_input"}})
                    return
                registry_release.update(
                    {
                        "release_id": release_id,
                        "project_id": project_id,
                        "schema_version": manifest["schemaVersion"],
                        "public_key": signature["publicKey"],
                        "content_hash": manifest["contentHash"],
                        "artifact_sha256": hashlib.sha256(artifact).hexdigest(),
                        "artifact_size": len(artifact),
                        "status": "submitted",
                        "revision": 1,
                        "build_status": "failed",
                        "build_revision": 2,
                    }
                )
                if (
                    "project_id" not in registry_project
                    or registry_release["project_id"]
                    != registry_project["project_id"]
                ):
                    self.send_json(409, {"error": {"code": "project_mismatch"}})
                    return
                self.send_json(
                    201,
                    {"created": True, "release": self.release("submitted", 1)},
                )
                return
            expected_path = (
                f"/v1/releases/{registry_release.get('release_id', '')}"
            )
            if (
                self.command == "POST"
                and self.path == expected_path + "/build/retry"
                and size == 0
            ):
                changed = registry_release.get("build_status") == "failed"
                if changed:
                    registry_release["build_status"] = "queued"
                    registry_release["build_revision"] += 1
                self.send_json(
                    200,
                    {"changed": changed, "build": self.build()},
                )
                return
            if self.command == "GET" and self.path == expected_path + "/build":
                if registry_release.get("build_status") == "queued":
                    registry_release["build_status"] = "succeeded"
                    registry_release["build_revision"] += 2
                self.send_json(200, {"build": self.build()})
                return
            if self.command == "GET" and self.path == expected_path:
                registry_release["status"] = "approved"
                registry_release["revision"] = 2
                self.send_json(200, {"release": self.release("approved", 2)})
                return
            if (
                self.command == "POST"
                and self.path == expected_path + "/publish"
                and size == 0
            ):
                registry_release["status"] = "published"
                registry_release["revision"] = 3
                self.send_json(
                    200,
                    {"changed": True, "release": self.release("published", 3)},
                )
                return
            self.send_json(404, {"error": {"code": "not_found"}})

        def handle(self):
            try:
                super().handle()
            except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
                pass

        do_POST = serve
        do_GET = serve
        do_PUT = serve
        do_DELETE = serve

    server = tls_server = registry_server = key_path = None
    threads = []
    fixture_flags = []
    if not args.parameter_ui:
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
        now = datetime.now(timezone.utc)
        certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                       .serial_number(x509.random_serial_number()).not_valid_before(now - timedelta(minutes=5))
                       .not_valid_after(now + timedelta(days=1)).add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
                       .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), critical=False).sign(key, hashes.SHA256()))
        cert_path, key_path = output / "fixture-ca.pem", output / "fixture-tls.key"
        cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        tls_server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        registry_server = ThreadingHTTPServer(("127.0.0.1", 0), RegistryHandler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert_path, key_path)
        tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True)
        threads = [
            threading.Thread(target=item.serve_forever, daemon=True)
            for item in (server, tls_server, registry_server)
        ]
        for thread in threads:
            thread.start()
        fixture_flags = [f"-CopilotFixture=http://127.0.0.1:{server.server_port}",
                         f"-CopilotTlsFixture=https://localhost:{tls_server.server_port}", f"-CopilotTlsCa={cert_path}",
                         f"-CopilotTlsMismatch=https://127.0.0.1:{tls_server.server_port}",
                         f"-RegistryFixture=http://127.0.0.1:{registry_server.server_port}"]
    log = output / "editor.log"
    selector = "AkUGC" if args.all_tests else "AkUGC.Editor.Copilot.RealHttpProtocol"
    command = [str(args.engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(root / "AkUGCPlatform.uproject"),
               *fixture_flags, f"-ExecCmds=Automation RunTests {selector}",
               "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-nosplash", "-nullrhi", "-stdout", f"-abslog={log}"]
    if args.generated_document:
        command += [f"-CopilotGeneratedDocument={args.generated_document}", f"-CopilotRuntimeOutput={output / 'generated-runtime.json'}"]
    if args.mouse_ui:
        command = mouse_ui_command(command, args, output, user_dir)
    try:
        with (output / "stdout.log").open("w", encoding="utf-8") as stream:
            import os
            child_env = dict(os.environ)
            child_env.pop("DEEPSEEK_API_KEY", None)
            deadline = time.monotonic() + (420 if args.manual_slider else 220)
            process = subprocess.Popen(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT, env=child_env)
            try:
                if args.mouse_ui:
                    base = f"http://127.0.0.1:{server.server_port}" if server else ""
                    drive_mouse_ui(process, output, base, args.menu_entry, args.mouse_live_authorized,
                                   args.parameter_ui, args.slider_ui, args.library_ui, args.manual_slider)
                code = process.wait(timeout=max(1, deadline - time.monotonic()))
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
    finally:
        for item in (server, tls_server, registry_server):
            if item is not None:
                item.shutdown()
                item.server_close()
        for thread in threads:
            thread.join(timeout=5)
        if key_path is not None:
            key_path.unlink(missing_ok=True)
    raw = log.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("gb18030", errors="replace")
    automation_text = (output / "stdout.log").read_text(
        encoding="utf-8", errors="replace"
    )
    completed = re.findall(r"\[AkUGCCopilotHTTP\] Case=([\w-]+) Complete=1", text)
    counts = Counter(event["path"] for event in events)
    actual_selector = (
        "AkUGC.Editor.Copilot.OSMouseInteraction" if args.mouse_ui else selector
    )
    expected_path = (
        actual_selector
        if args.mouse_ui or not args.all_tests
        else "AkUGC.Editor.Copilot.RealHttpProtocol"
    )
    automation_verdict = None
    automation_error = ""
    try:
        automation_verdict = require_automation_success(
            automation_text,
            expected_selector=actual_selector,
            expected_paths=(expected_path,),
            expected_total=None if args.all_tests and not args.mouse_ui else 1,
            minimum_total=137 if args.all_tests and not args.mouse_ui else None,
            require_queue_empty=True,
            require_test_exit=True,
            return_code=code,
        )
    except AutomationVerdictError as error:
        automation_error = str(error)
    if args.mouse_ui:
        result = json.loads((output / "ui-result.json").read_text(encoding="utf-8"))
        if args.menu_entry or args.parameter_ui or args.library_ui:
            mode_passed = not events
            if args.library_ui:
                actions = json.loads((output / "os-input-actions.json").read_text(encoding="utf-8"))
                clicks = Counter(a.get("label") for a in actions if a.get("action") == "mouse_click" and a.get("slate_hit_verified"))
                mode_passed = mode_passed and clicks["一键签名发布当前项目"] == 1 and clicks["取消删除"] == 1
                mode_passed = mode_passed and clicks["确认删除记录"] == 1 and clicks["验签并导入作品库"] == 3
                mode_passed = mode_passed and any(a.get("action") == "retained_share_sha256_verified" for a in actions)
        elif args.mouse_live_authorized:
            actions = json.loads((output / "os-input-actions.json").read_text(encoding="utf-8"))
            mode_passed = result.get("last_http_code") == 200 and not events and sum(a.get("label") == "发送需求并生成预览" for a in actions) == 1
        else:
            mode_passed = all(counts[path] == 1 for path in ("/ok", "/delayed", "/ui-invalid", "/explain-ok"))
        result["passed"] = result["passed"] and not automation_error and mode_passed
        result["automation_verdict_error"] = automation_error
        result["automation_tests_completed"] = (
            len(automation_verdict.completions) if automation_verdict else 0
        )
        result["http_requests"] = events
        result["real_model_called"] = args.mouse_live_authorized
        result["model_calls_budget"] = 1 if args.mouse_live_authorized else 0
        result["automated_os_input"] = True
        result.update(slider_result_details(output, args, completed=bool(result["passed"])))
        if args.slider_ui or args.manual_slider:
            result["passed"] = result["passed"] and result["numeric_slider_verified"]
        result["actions"] = read_json(output / "os-input-actions.json")
        write_json(output / "result.json", result)
        print(output / "result.json")
        print("mouse_ui_passed=", result["passed"])
        if not result["passed"]:
            raise RuntimeError("Mouse UI validation failed")
        return
    registry_project_path = (
        f"/v1/projects/{registry_release.get('project_id', '')}"
    )
    registry_release_path = (
        f"/v1/releases/{registry_release.get('release_id', '')}"
    )
    registry_expected_sequence = [
        ("GET", registry_project_path),
        ("POST", "/v1/projects"),
        ("POST", "/v1/releases"),
        ("GET", registry_release_path + "/build"),
        ("POST", registry_release_path + "/build/retry"),
        ("GET", registry_release_path),
        ("GET", registry_release_path + "/build"),
        ("GET", registry_project_path),
        ("GET", registry_project_path + "/members"),
        ("GET", registry_project_path + "/versions?limit=100"),
        ("PUT", registry_project_path + "/members/editor-b"),
        ("GET", registry_project_path),
        ("GET", registry_project_path + "/members"),
        ("GET", registry_project_path + "/versions?limit=100"),
        ("DELETE", registry_project_path + "/members/editor-b"),
        ("GET", registry_project_path),
        ("GET", registry_project_path + "/members"),
        ("GET", registry_project_path + "/versions?limit=100"),
        ("POST", registry_release_path + "/publish"),
        ("PUT", registry_project_path + "/visibility"),
        ("GET", registry_project_path),
        ("GET", registry_project_path + "/members"),
        ("GET", registry_project_path + "/versions?limit=100"),
    ]
    registry_protocol_passed = (
        not args.all_tests
        or (
            "[AkUGCReleaseRegistryHTTP] Passed Operations=23 LoopbackOnly=1 Visibility=public Build=succeeded BuildRetry=1" in text
            and [
                (event["method"], event["path"])
                for event in registry_events
            ] == registry_expected_sequence
            and registry_events[1]["content_type"] == "application/json"
            and registry_events[1]["idempotency_key"].startswith(
                "akugc-project-"
            )
            and registry_events[2]["content_type"]
                == "application/vnd.akugc+json"
            and registry_events[2]["idempotency_key"].startswith(
                "akugc-release-"
            )
            and registry_events[2]["content_length"] > 0
            and registry_events[4]["content_length"] == 0
            and registry_events[10]["content_type"] == "application/json"
            and registry_events[10]["content_length"] > 0
            and registry_events[14]["content_length"] == 0
            and registry_events[18]["content_length"] == 0
            and all(event["authorization_valid"] for event in registry_events)
        )
    )
    result = {"kind": "local_http_protocol_fixture_not_real_ai", "passed": not automation_error
              and "[AkUGCCopilotHTTP] Passed Cases=42 FixtureOnly=1" in text
              and registry_protocol_passed,
              "cases": completed, "http_requests": events,
              "registry_requests": registry_events,
              "creator_registry_protocol_verified": registry_protocol_passed,
              "credentials_only_sent_over_verified_tls": all(not event["authorization_present"] or event["tls"] for event in events)
                  and "/credentials-must-not-arrive" not in counts,
              "tls_authenticated_success": any(event["path"] == "/tls-ok" and event["authorization_valid_fixture"] for event in events),
              "tls_invalid_certificates_rejected_before_http": "/tls-untrusted" not in counts and "/tls-hostname" not in counts,
              "request_level_redirect_blocked": all(path in counts for path in ("/redirect", "/redirect-307", "/tls-redirect302", "/tls-redirect307", "/tls-redirect-downgrade")) and "/redirect-target" not in counts,
              "native_timeout_verified": "[AkUGCCopilotHTTP] NativeTimeout=1" in text and "/timeout-delayed" in counts,
              "old_response_resources": 777, "new_response_resources": 300,
              "retired_callback_injection": "identity guard tested while pending and after preview",
              "controlled_graph_protocol_verified": all(path in counts for path in ("/graph", "/graph-cycle", "/graph-types")),
              "explanation_diagnostic_only": all(path in counts for path in ("/explain-ok", "/explain-types", "/explain-extra", "/explain-stale", "/explain-cancel", "/explain-duplicate", "/explain-long")),
              "private_diagnostic_redaction_verified": all(path in counts for path in ("/explain-private-field", "/explain-private-duplicate")),
              "outer_duplicate_response_rejected": all(case in completed for case in ("outer-duplicate-choices", "outer-duplicate-content")),
              "unsent_generation_preservation_tested": "ok" in completed and "explain-ok" in completed,
              "real_model_validated": False,
              "automation_passed": len(automation_verdict.completions) if automation_verdict else 0,
              "automation_verdict_error": automation_error,
              "log": str(log)}
    if args.generated_document:
        runtime_path = output / "generated-runtime.json"
        runtime = json.loads(runtime_path.read_text(encoding="utf-8")) if runtime_path.is_file() else {}
        result["generated_document_runtime"] = runtime
        result["passed"] = result["passed"] and runtime.get("passed", False)
    (output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(output / "result.json")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if (not result["passed"] or not result["credentials_only_sent_over_verified_tls"] or len(set(completed)) != 42
            or not result["request_level_redirect_blocked"] or not result["native_timeout_verified"]
            or not result["tls_authenticated_success"] or not result["tls_invalid_certificates_rejected_before_http"]
            or not result["explanation_diagnostic_only"]
            or not result["creator_registry_protocol_verified"]):
        raise RuntimeError(f"Copilot HTTP protocol verification failed: {log}\n{text[-5000:]}")


if __name__ == "__main__":
    main()
