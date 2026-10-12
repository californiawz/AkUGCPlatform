import argparse
from collections import Counter
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


def drive_mouse_ui(process, output, base, menu=False, live=False, parameters=False, slider=False, library=False):
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
    user.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user.FindWindowW.restype = wintypes.HWND
    title = "AkUGC Copilot OS Mouse Verification"
    actions = []

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
        try:
            return json.loads((output / name).read_text(encoding="utf-8"))
        except (FileNotFoundError, PermissionError, json.JSONDecodeError):
            return {}

    def wait(check, timeout=30):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if process.poll() is not None:
                raise RuntimeError("Editor exited before OS UI validation finished")
            state = read()
            if check(state):
                return state
            time.sleep(0.1)
        raise TimeoutError("OS UI expected state not reached")

    if library:
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
        user.IsWindowVisible.argtypes = [wintypes.HWND]
        end = time.monotonic() + 100
        while time.monotonic() < end and not read().get("controls"):
            def focus_owned(window, _):
                owner = wintypes.DWORD()
                user.GetWindowThreadProcessId(window, ctypes.byref(owner))
                rect = wintypes.RECT()
                if owner.value == process.pid and user.IsWindowVisible(window) and user.GetWindowRect(window, ctypes.byref(rect)):
                    if rect.right - rect.left > 800 and rect.bottom - rect.top > 600:
                        user.SetForegroundWindow(window)
                return True
            user.EnumWindows(callback_type(focus_owned), 0)
            if process.poll() is not None:
                raise RuntimeError("Editor exited during library startup")
            time.sleep(0.5)
    state = wait(lambda s: bool(s.get("controls")), 100)
    if menu:
        title = state["window_title"]
    window = user.FindWindowW(None, title)
    pid = wintypes.DWORD()
    if not window or not user.GetWindowThreadProcessId(window, ctypes.byref(pid)) or pid.value != process.pid:
        raise RuntimeError("Verification Slate window not found in the launched Editor process")
    user.SetForegroundWindow(window)
    time.sleep(0.5)

    def foreground():
        active = user.GetForegroundWindow()
        active_pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(active, ctypes.byref(active_pid))
        if (not menu and active != window) or active_pid.value != process.pid:
            raise RuntimeError("Verification window lost foreground; stopped without sending input elsewhere")

    def position_cursor(x, y):
        foreground()
        state = wait(lambda s: bool(s.get("controls")), 5)
        rect = wintypes.RECT()
        if not user.GetWindowRect(window, ctypes.byref(rect)):
            raise ctypes.WinError(ctypes.get_last_error())
        physical_x = rect.left + (x - state["window_x"]) * (rect.right - rect.left) / state["window_width"]
        physical_y = rect.top + (y - state["window_y"]) * (rect.bottom - rect.top) / state["window_height"]
        user.SetCursorPos(int(physical_x), int(physical_y))
        foreground()

    def control(label, kind="SButton", scroll=False):
        for _ in range(24 if scroll else 1):
            state = wait(lambda s: bool(s.get("controls")), 5)
            candidates = [c for c in state.get("controls", []) if (c["type"] == kind or kind == "SNumericEntryBox" and c["type"].startswith(kind)) and label in c["label"]
                          and c["width"] > 1 and c["height"] > 1
                          and state["window_x"] + 5 < c["x"] < state["window_x"] + state["window_width"] - 5
                          and state["window_y"] + (0 if menu else 60) < c["y"] < state["window_y"] + state["window_height"] - (0 if menu else 90)]
            if candidates:
                candidate = candidates[0]
                if not candidate["enabled"]:
                    return candidate
                position_cursor(candidate["x"], candidate["y"])
                time.sleep(0.2)
                if any(p.startswith(candidate["type"] + ":") and label in p for p in read().get("hover_path", [])):
                    return candidate
            if not scroll:
                break
            position_cursor(state["window_x"] + state["window_width"] * 0.8,
                            state["window_y"] + state["window_height"] * 0.75)
            user.mouse_event(0x0800, 0, 0, ctypes.c_uint32(-360).value, 0)
            time.sleep(0.3)
        raise RuntimeError(f"Visible UI control not found: {label}")

    def click(label, kind="SButton", scroll=False):
        c = control(label, kind, scroll)
        if not c["enabled"]:
            raise RuntimeError(f"UI control unexpectedly disabled: {label}")
        position_cursor(c["x"], c["y"])
        wait(lambda s: any(p.startswith(c["type"] + ":") and label in p for p in s.get("hover_path", [])), 3)
        foreground()
        user.mouse_event(0x0002, 0, 0, 0, 0)
        user.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(0.35)
        actions.append({"action": "mouse_click", "label": label, "x": c["x"], "y": c["y"],
                        "slate_hit_verified": True})

    def fill(label, text, kind="SEditableTextBox"):
        click(label, kind, not parameters)
        foreground()
        user.keybd_event(0x11, 0, 0, 0)
        user.keybd_event(0x41, 0, 0, 0)
        user.keybd_event(0x41, 0, 2, 0)
        user.keybd_event(0x11, 0, 2, 0)
        user.keybd_event(0x08, 0, 0, 0)
        user.keybd_event(0x08, 0, 2, 0)
        for ch in text:
            foreground()
            events = (Input * 2)()
            events[0].type = events[1].type = 1
            events[0].value.keyboard = Keyboard(0, ord(ch), 4, 0, 0)
            events[1].value.keyboard = Keyboard(0, ord(ch), 6, 0, 0)
            if user.SendInput(2, events, ctypes.sizeof(Input)) != 2:
                raise ctypes.WinError(ctypes.get_last_error())
        if kind == "SNumericEntryBox":
            foreground()
            user.keybd_event(0x0D, 0, 0, 0)
            user.keybd_event(0x0D, 0, 2, 0)
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
        click(window_label, "STextBlock")
        wait(lambda s: any(c["label"] == "AkUGC Creator Studio" for c in s.get("controls", [])))
        click("AkUGC Creator Studio", "STextBlock")
        wait(lambda s: s.get("creator_tab_open"))
        checkpoint("menu")
        checkpoint("finish")
        (output / "os-input-actions.json").write_text(json.dumps(actions, ensure_ascii=False, indent=2), encoding="utf-8")
        return

    if library:
        state = read()
        for name in ("library_directory", "project_path"):
            if not Path(state[name]).resolve().is_relative_to(output.parent.resolve()):
                raise RuntimeError("Library test path escaped isolated Saved root")
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
        fill("基地生命", "1700", "SNumericEntryBox")
        fill("初始资源", "425", "SNumericEntryBox")
        fill("波数", "2", "SNumericEntryBox")
        if slider:
            c = control("波数", "SNumericEntryBox")
            position_cursor(c["x"], c["y"])
            wait(lambda s: any(p.startswith(c["type"] + ":") and "波数" in p for p in s.get("hover_path", [])), 3)
            foreground()
            user.mouse_event(0x0002, 0, 0, 0, 0)
            try:
                user.keybd_event(0x10, 0, 0, 0)
                try:
                    for _ in range(10):
                        foreground()
                        user.mouse_event(0x0001, 100, 0, 0, 0)
                        time.sleep(0.08)
                    wait(lambda s: s.get("draft_waves") == 3 and s.get("mouse_captured"), 3)
                finally:
                    foreground()
                    user.keybd_event(0x10, 0, 2, 0)
            finally:
                foreground()
                user.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(0.5)
            wait(lambda s: s.get("draft_waves") == 3 and not s.get("mouse_captured"), 3)
            actions.append({"action": "numeric_slider_drag", "label": "波数", "slate_hit_verified": True,
                            "active_capture_observed": True, "released_value_preserved": True})
            fill("波数", "2", "SNumericEntryBox")
        wait(lambda s: s.get("draft_waves") == 2 and s.get("draft_resources") == 425 and s.get("draft_base_health") == 1700)
        checkpoint("draft")
        click("Save")
        checkpoint("saved-draft")
        click("应用参数", scroll=True)
        wait(lambda s: s.get("can_undo"))
        checkpoint("parameters-applied")
        click("Undo")
        wait(lambda s: not s.get("can_undo") and s.get("draft_waves") == 3)
        checkpoint("parameters-undo")
        click("Redo")
        wait(lambda s: s.get("can_undo") and s.get("draft_resources") == 425)
        checkpoint("parameters-redo")
        click("Save")
        checkpoint("saved-applied")
        click("Load")
        wait(lambda s: not s.get("can_undo") and not s.get("can_redo"))
        checkpoint("reloaded")
        fill("初始资源", "999", "SNumericEntryBox")
        wait(lambda s: s.get("draft_resources") == 999)
        click("重新读取项目 / 放弃草稿", scroll=True)
        wait(lambda s: s.get("draft_resources") == 425)
        checkpoint("discarded")
        checkpoint("finish")
        (output / "os-input-actions.json").write_text(json.dumps(actions, ensure_ascii=False, indent=2), encoding="utf-8")
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--all-tests", action="store_true")
    parser.add_argument("--generated-document", type=Path)
    parser.add_argument("--mouse-ui", action="store_true")
    parser.add_argument("--menu-entry", action="store_true")
    parser.add_argument("--mouse-live-authorized", action="store_true")
    parser.add_argument("--parameter-ui", action="store_true")
    parser.add_argument("--slider-ui", action="store_true")
    parser.add_argument("--library-ui", action="store_true")
    args = parser.parse_args()
    if args.slider_ui and not args.parameter_ui:
        parser.error("--slider-ui requires --parameter-ui")
    if (args.menu_entry or args.mouse_live_authorized or args.parameter_ui or args.library_ui) and not args.mouse_ui:
        parser.error("Menu, live, parameter and library modes require --mouse-ui")
    if sum((args.menu_entry, args.mouse_live_authorized, args.parameter_ui, args.library_ui)) > 1:
        parser.error("Menu, live and parameter modes run separately")
    if args.mouse_live_authorized:
        import os
        if not os.environ.get("DEEPSEEK_API_KEY"):
            parser.error("Live OS input requires process key")
    if args.mouse_ui and (args.all_tests or args.generated_document):
        parser.error("--mouse-ui runs separately from headless full regression")
    root = Path(__file__).resolve().parents[2]
    if args.generated_document:
        args.generated_document = args.generated_document.resolve()
        if not args.all_tests or not args.generated_document.is_file() or not args.generated_document.is_relative_to(root / "Saved"):
            parser.error("--generated-document requires --all-tests and an existing document under project Saved")
    output = root / "Saved/CopilotHttpVerification" / uuid.uuid4().hex
    user_dir = output / "editor-user"
    if args.library_ui:
        output = user_dir / "Saved/verification"
    output.mkdir(parents=True)
    events = []
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
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert_path, key_path)
    tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True)
    threads = [threading.Thread(target=item.serve_forever, daemon=True) for item in (server, tls_server)]
    for thread in threads:
        thread.start()
    log = output / "editor.log"
    selector = "AkUGC" if args.all_tests else "AkUGC.Editor.Copilot.RealHttpProtocol"
    command = [str(args.engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(root / "AkUGCPlatform.uproject"),
               f"-CopilotFixture=http://127.0.0.1:{server.server_port}",
               f"-CopilotTlsFixture=https://localhost:{tls_server.server_port}", f"-CopilotTlsCa={cert_path}",
               f"-CopilotTlsMismatch=https://127.0.0.1:{tls_server.server_port}", f"-ExecCmds=Automation RunTests {selector}",
               "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-nosplash", "-nullrhi", "-stdout", f"-abslog={log}"]
    if args.generated_document:
        command += [f"-CopilotGeneratedDocument={args.generated_document}", f"-CopilotRuntimeOutput={output / 'generated-runtime.json'}"]
    if args.mouse_ui:
        command[0] = str(args.engine / "Engine/Binaries/Win64/UnrealEditor.exe")
        command = [part for part in command if part != "-nullrhi" and not part.startswith("-ExecCmds=")]
        ui_selector = "Automation RunTests AkUGC.Editor.Copilot.OSMouseInteraction"
        if args.slider_ui:
            ui_selector = "Slate.ForceRawInputSimulation 1," + ui_selector
        command += [f"-ExecCmds={ui_selector}", f"-CopilotMouseOutput={output}"]
        if args.menu_entry:
            command += ["-CopilotMenuEntry"]
        if args.mouse_live_authorized:
            command += ["-CopilotMouseLive", "-CopilotLiveAuthorized"]
        if args.parameter_ui:
            command += ["-CreatorParameterMouse"]
        if args.library_ui:
            command = [part for part in command if part != "-unattended"]
            command += ["-CreatorLibraryMouse", "-UnattendedInput", f"-UserDir={user_dir}"]
    try:
        with (output / "stdout.log").open("w", encoding="utf-8") as stream:
            import os
            child_env = dict(os.environ)
            child_env.pop("DEEPSEEK_API_KEY", None)
            process = subprocess.Popen(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT, env=child_env)
            try:
                if args.mouse_ui:
                    drive_mouse_ui(process, output, f"http://127.0.0.1:{server.server_port}", args.menu_entry, args.mouse_live_authorized, args.parameter_ui, args.slider_ui, args.library_ui)
                code = process.wait(timeout=220)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
    finally:
        for item in (server, tls_server):
            item.shutdown()
            item.server_close()
        for thread in threads:
            thread.join(timeout=5)
        key_path.unlink(missing_ok=True)
    raw = log.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("gb18030", errors="replace")
    completed = re.findall(r"\[AkUGCCopilotHTTP\] Case=([\w-]+) Complete=1", text)
    counts = Counter(event["path"] for event in events)
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
        result["passed"] = result["passed"] and code == 0 and "Result={Fail}" not in text and mode_passed
        result["http_requests"] = events
        result["real_model_called"] = args.mouse_live_authorized
        result["model_calls_budget"] = 1 if args.mouse_live_authorized else 0
        result["automated_os_input"] = True
        result["numeric_slider_verified"] = args.slider_ui and any(a.get("action") == "numeric_slider_drag" for a in json.loads((output / "os-input-actions.json").read_text(encoding="utf-8")))
        (output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(output / "result.json")
        print("mouse_ui_passed=", result["passed"])
        if not result["passed"]:
            raise RuntimeError("Mouse UI validation failed")
        return
    result = {"kind": "local_http_protocol_fixture_not_real_ai", "passed": code == 0 and "Result={Fail}" not in text
              and "[AkUGCCopilotHTTP] Passed Cases=42 FixtureOnly=1" in text,
              "cases": completed, "http_requests": events,
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
              "automation_passed": len(re.findall(r"Test Completed\. Result=\{Success\}", text)),
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
            or not result["explanation_diagnostic_only"]):
        raise RuntimeError(f"Copilot HTTP protocol verification failed: {log}\n{text[-5000:]}")


if __name__ == "__main__":
    main()
