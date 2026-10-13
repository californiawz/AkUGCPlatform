import argparse
import json
from pathlib import Path
import re
import subprocess
import time
import uuid
import socket

from verify_network_visuals import read_log, run_checked, verify_lua_startup, wait_for


def run_cook_checked(args, path, timeout):
    import ctypes
    from ctypes import wintypes
    import sys

    class BasicLimits(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
                    ("flags", wintypes.DWORD), ("min_working_set", ctypes.c_size_t),
                    ("max_working_set", ctypes.c_size_t), ("active_process_limit", wintypes.DWORD),
                    ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("basic", BasicLimits), ("io", IoCounters),
                    ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                    ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    process = None
    try:
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000
        if not kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())
        runner = "import json,subprocess,sys; sys.stdin.buffer.read(1); sys.exit(subprocess.call(json.loads(sys.argv[1])))"
        with path.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen([sys.executable, "-c", runner, json.dumps(args)], stdin=subprocess.PIPE,
                                       stdout=stream, stderr=subprocess.STDOUT)
            if not kernel.AssignProcessToJobObject(job, wintypes.HANDLE(int(process._handle))):
                raise ctypes.WinError(ctypes.get_last_error())
            process.stdin.write(b"1")
            process.stdin.close()
            code = process.wait(timeout=timeout)
        if code:
            raise RuntimeError(f"Cook returned {code}: {path}\n{read_log(path)[-5000:]}")
    finally:
        kernel.CloseHandle(job)
        if process is not None:
            if process.stdin is not None and not process.stdin.closed:
                process.stdin.close()
            if process.poll() is None:
                process.kill()
            process.wait(timeout=15)


def verify_recovery(engine, root, output, fault, packaged=False, cook=False, host_entry_ui=False):
    editor = engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    project = root / "AkUGCPlatform.uproject"
    game = root / "Saved/StagedBuilds/Windows/AkUGCPlatform/Binaries/Win64/AkUGCPlatform.exe"
    client = root / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
    if packaged:
        if cook:
            uat = engine / "Engine/Build/BatchFiles/RunUAT.bat"
            for mode, target in (("game", "AkUGCPlatform"), ("client", "AkUGCPlatformClient")):
                args = [str(uat), "BuildCookRun", f"-project={project}", "-noP4", "-unattended", "-utf8output",
                        "-platform=Win64", f"-target={target}", "-clientconfig=Development",
                        "-build", "-cook", "-stage", "-pak", "-package"]
                if mode == "client":
                    args += ["-client", "-noserver"]
                run_cook_checked(args, output / f"cook_{mode}.log", 3600)
        for executable in (game, client):
            if not executable.is_file():
                raise RuntimeError(f"Missing complete staged package executable: {executable}")
            paks = executable.parents[2] / "Content/Paks"
            if not paks.is_dir() or not any(paks.glob("*.pak")):
                raise RuntimeError(f"Missing package content: {paks}")
    receiver_user = output / "receiver-user"
    ready = receiver_user / "Saved/receiver.ready" if packaged else output / "receiver.ready"
    ready.parent.mkdir(parents=True, exist_ok=True)
    processes, handles = [], []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    def launch(url, name, args):
        log = output / f"{name}.log"
        handle = (output / f"{name}.stdout.log").open("w", encoding="utf-8")
        handles.append(handle)
        prefix = [str(game if name == "host" else client)] if packaged else [str(editor), str(project), "-game"]
        isolation = [f"-UserDir={output / 'host-user' if name == 'host' else receiver_user}"] if packaged else []
        rendering = ["-windowed", "-ResX=1280", "-ResY=800"] if host_entry_ui and name == "host" else ["-nullrhi"]
        process = subprocess.Popen([*prefix, url, "-unattended", "-nop4", "-nosplash",
                                    *rendering, "-stdout", f"-abslog={log}", *isolation, *args], cwd=root,
                                   stdout=handle, stderr=subprocess.STDOUT)
        processes.append(process)
        return process, log

    def wait_for(process, path, marker, timeout):
        deadline = time.monotonic() + timeout
        stdout = path.with_suffix(".stdout.log")
        while time.monotonic() < deadline:
            text = read_log(path) + "\n" + read_log(stdout)
            if "Result={Fail}" in text or "Authority Logic Pack load failed" in text:
                raise RuntimeError(f"Recovery failed before {marker}: {path}\n{text[-4000:]}")
            if marker in text:
                return text
            if process.poll() is not None:
                text = read_log(path) + "\n" + read_log(stdout)
                if marker in text and "Result={Fail}" not in text:
                    return text
                raise RuntimeError(f"Recovery process exited before {marker}: {path}\n{text[-4000:]}")
            time.sleep(0.25)
        raise RuntimeError(f"Recovery timed out before {marker}: {path}")

    ui_actions = []

    def game_window(process):
        import ctypes
        from ctypes import wintypes
        user = ctypes.WinDLL("user32", use_last_error=True)
        user.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user.IsWindowVisible.argtypes = [wintypes.HWND]
        user.GetForegroundWindow.restype = wintypes.HWND
        user.SetForegroundWindow.argtypes = [wintypes.HWND]
        user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        user.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
        matches = []

        def visit(window, _):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(window, ctypes.byref(pid))
            rect = wintypes.RECT()
            if pid.value == process.pid and user.IsWindowVisible(window) and user.GetClientRect(window, ctypes.byref(rect)):
                if rect.right > 600 and rect.bottom > 400:
                    matches.append(window)
            return True

        user.EnumWindows(callback_type(visit), 0)
        if len(matches) != 1:
            raise RuntimeError("Expected one visible Game viewport owned by launched host PID")
        return user, matches[0]

    def capture_game(process, tag):
        import ctypes
        from ctypes import wintypes
        from PIL import ImageGrab
        user, window = game_window(process)
        if not user.SetForegroundWindow(window) and user.GetForegroundWindow() != window:
            raise RuntimeError("Cannot focus Game viewport; refusing OS input")
        time.sleep(0.5)
        if user.GetForegroundWindow() != window:
            raise RuntimeError("Game viewport lost foreground")
        rect, origin = wintypes.RECT(), wintypes.POINT()
        if not user.GetClientRect(window, ctypes.byref(rect)) or not user.ClientToScreen(window, ctypes.byref(origin)):
            raise ctypes.WinError(ctypes.get_last_error())
        ImageGrab.grab(bbox=(origin.x, origin.y, origin.x + rect.right, origin.y + rect.bottom), all_screens=True).save(output / f"{tag}.png")
        ui_actions.append({"action": "viewport_capture", "tag": tag, "host_pid": process.pid})
        return user, window, origin.x + rect.right // 2, origin.y + rect.bottom // 2

    test = ["-AkUGCRoomRecovery", f"-RoomRecoveryReady={ready}",
            "-ExecCmds=Automation RunTests AkUGC.Runtime.Room.FaultRecovery"]
    try:
        host = launch("/Game/Maps/Phase0" if host_entry_ui else "/Game/Maps/Phase0?listen?room=1", "host",
                      [f"-port={port}", *([] if host_entry_ui else ["-AkUGCRoom"]),
                       * (test + ["-RoomRecoveryFault=leave"] if fault == "leave" else [])])
        if host_entry_ui:
            initial = wait_for(*host, "[AkUGCRoomRecovery] Target Editor=0 Client=0 Server=0", 100)
            if "IpNetDriver listening on port" in initial or "[AkUGCRoom] Joined" in initial:
                raise RuntimeError("Game was already hosting before OS F6 input")
            user, window, x, y = capture_game(host[0], "game-local-before-f6")
            user.SetCursorPos(x, y)
            if user.GetForegroundWindow() != window:
                raise RuntimeError("Stopped before sending input to a different foreground window")
            user.mouse_event(0x0002, 0, 0, 0, 0)
            user.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(0.25)
            if user.GetForegroundWindow() != window:
                raise RuntimeError("Stopped before sending F6 to a different foreground window")
            user.keybd_event(0x75, 0, 0, 0)
            user.keybd_event(0x75, 0, 2, 0)
            ui_actions.append({"action": "os_key", "key": "F6", "host_pid": host[0].pid, "foreground_verified": True})
        listening = wait_for(*host, "IpNetDriver listening on port", 60)
        ports = re.findall(r"IpNetDriver listening on port (\d+)", listening)
        if not ports or int(ports[-1]) != port:
            raise RuntimeError("Host bound a different port; refusing to connect to an unrelated process")
        if host_entry_ui:
            wait_for(*host, "[AkUGCRoom] Joined Id=1 Members=1", 30)
            capture_game(host[0], "game-room-after-f6")
        receiver = launch(f"127.0.0.1:{port}", "receiver",
                          [*test, "-AkUGCRoom", f"-RoomRecoveryFault={fault}", "-TestExit=Automation Test Queue Empty"])
        wait_for(*receiver, "[AkUGCRoomRecovery] Joined", 60)
        if fault == "crash":
            host[0].terminate()
            host[0].wait(timeout=15)
        if fault == "leave":
            wait_for(*host, "[AkUGCRoomRecovery] HostLeaveIssued", 35)
            host_text = wait_for(*host, "Test Completed. Result={Success} Name={FaultRecovery}", 35)
            if "[AkUGCRoomRecovery] Passed CleanLocal=1 SoloStartReturn=1" not in host_text:
                raise RuntimeError("Host did not recover to operable local preparation")
            if host_entry_ui:
                capture_game(host[0], "game-recovered-local")
        wait_for(*receiver, "Test Completed. Result={Success} Name={FaultRecovery}", 105)
        code = receiver[0].wait(timeout=20)
        if code:
            raise RuntimeError(f"Recovered receiver exited with code {code}")
        text = read_log(receiver[1]) + "\n" + read_log(receiver[1].with_suffix(".stdout.log"))
        if "Result={Fail}" in text or "Test Completed. Result={Success} Name={FaultRecovery}" not in text:
            raise RuntimeError("Final receiver logs did not confirm automation success")
        if fault == "leave":
            host_text = read_log(host[1]) + "\n" + read_log(host[1].with_suffix(".stdout.log"))
            if "Result={Fail}" in host_text or "Test Completed. Result={Success} Name={FaultRecovery}" not in host_text:
                raise RuntimeError("Final host logs did not confirm automation success")
        if "[AkUGCRoomRecovery] Passed CleanLocal=1 SoloStartReturn=1" not in text:
            raise RuntimeError("Missing clean operable local recovery evidence")
        if fault == "timeout" and ("BlackholeApplied" not in text or "FailureType = ConnectionTimeout" not in text):
            raise RuntimeError("Silent loss did not actually trigger connection timeout")
        if fault != "timeout" and "FailureType = ConnectionLost" not in text and "FailureType = ConnectionTimeout" not in text:
            raise RuntimeError("No actual connection failure observed")
        if packaged:
            if "Target Editor=0 Client=1 Server=0" not in text:
                raise RuntimeError("Receiver was not the packaged Client target")
            if fault == "leave" and "Target Editor=0 Client=0 Server=0" not in host_text:
                raise RuntimeError("Host was not the packaged Game target")
        if fault != "crash" and host[0].poll() is not None:
            raise RuntimeError("Host exited unexpectedly before final recovery evidence")
        if host_entry_ui:
            (output / "os-input-actions.json").write_text(json.dumps(ui_actions, ensure_ascii=False, indent=2), encoding="utf-8")
        result = {"passed": True, "fault": fault, "startup_room_flag": not host_entry_ui,
                  "receiver_startup_room_flag": True, "game_host_f6_os_input_verified": host_entry_ui,
                  "human_ui_validated": False, "host_rendered": host_entry_ui,
                  "same_machine_not_cross_device": True, "editor_game_not_packaged_game": not packaged,
                  "packaged_game_and_client": packaged, "fresh_cook": cook,
                  "host_executable": str(game if packaged else editor), "receiver_executable": str(client if packaged else editor),
                  "clean_local_preparation": True, "solo_start_and_return": True,
                  "port": port, "host_port_verified": True,
                  "evidence": list(dict.fromkeys(line for line in text.splitlines() if "[AkUGCRoomRecovery]" in line or "FailureType =" in line))}
        path = output / "recovery_result.json"
        path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(path)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=15)
        for handle in handles:
            handle.close()


def verify_keyboard_rounds(engine, root, output, cook=False, render=False):
    import ctypes
    from ctypes import wintypes
    from PIL import ImageGrab
    if (output / "keyboard_result.json").exists() or (output / "host").exists() or (output / "receiver").exists():
        raise RuntimeError("Keyboard flow requires a fresh output directory")
    game = root / "Saved/StagedBuilds/Windows/AkUGCPlatform/Binaries/Win64/AkUGCPlatform.exe"
    client = root / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
    if cook:
        for target, mode in (("AkUGCPlatform", "game"), ("AkUGCPlatformClient", "client")):
            args = [str(engine / "Engine/Build/BatchFiles/RunUAT.bat"), "BuildCookRun", f"-project={root / 'AkUGCPlatform.uproject'}",
                    "-noP4", "-unattended", "-utf8output", "-platform=Win64", f"-target={target}",
                    "-clientconfig=Development", "-build", "-cook", "-stage", "-pak", "-package"]
            if mode == "client":
                args += ["-client", "-noserver"]
            run_cook_checked(args, output / f"cook_{mode}.log", 3600)
    for exe in (game, client):
        if not exe.is_file() or not any((exe.parents[2] / "Content/Paks").glob("*.pak")):
            raise RuntimeError(f"Missing complete package: {exe}")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetForegroundWindow.restype = wintypes.HWND
    user.SetForegroundWindow.argtypes = [wintypes.HWND]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
    user.VkKeyScanW.argtypes = [wintypes.WCHAR]
    user.VkKeyScanW.restype = ctypes.c_short
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    class JobLimits(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
                    ("flags", wintypes.DWORD), ("min_working_set", ctypes.c_size_t),
                    ("max_working_set", ctypes.c_size_t), ("active_process_limit", wintypes.DWORD),
                    ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]
    class JobIoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]
    class JobExtendedLimits(ctypes.Structure):
        _fields_ = [("basic", JobLimits), ("io", JobIoCounters),
                    ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                    ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = JobExtendedLimits()
    limits.basic.flags = 0x2000
    if not kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
        error = ctypes.get_last_error()
        kernel.CloseHandle(job)
        raise ctypes.WinError(error)
    processes, streams, actions, snapshots = [], [], [], []

    def launch(exe, name, x):
        directory = output / name
        state_dir = directory / "Saved/keyboard"
        state_dir.mkdir(parents=True)
        stream = (directory / "stdout.log").open("w", encoding="utf-8")
        streams.append(stream)
        command = [str(exe), "/Game/Maps/Phase0", f"-port={port}", "-windowed", "-ResX=1000", "-ResY=700",
                   f"-WinX={x}", "-WinY=60", "-unattended", "-UnattendedInput", "-nop4", "-nosplash", "-stdout",
                   f"-UserDir={directory}", f"-abslog={directory / 'process.log'}", f"-RoomKeyboardOutput={state_dir}",
                   "-ExecCmds=Automation RunTests AkUGC.Runtime.Room.OSKeyboardTwoRounds"]
        if render:
            command.append(f"-AkUGCSceneVisual={state_dir / 'scene'}")
        process = subprocess.Popen(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT)
        processes.append(process)
        if not kernel.AssignProcessToJobObject(job, wintypes.HANDLE(int(process._handle))):
            raise ctypes.WinError(ctypes.get_last_error())
        return {"process": process, "dir": directory, "state_dir": state_dir, "seq": 0, "name": name}

    def state(player):
        try:
            return json.loads((player["state_dir"] / "state.json").read_text(encoding="utf-8"))
        except (FileNotFoundError, PermissionError, json.JSONDecodeError):
            return {}

    def logs(player):
        return read_log(player["dir"] / "process.log") + "\n" + read_log(player["dir"] / "stdout.log")

    def wait(player, predicate, timeout=35):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if player["process"].poll() is not None or "Result={Fail}" in logs(player):
                raise RuntimeError(f"Keyboard flow process failed: {player['dir']}")
            value = state(player)
            if value and predicate(value):
                return value
            time.sleep(0.1)
        raise RuntimeError(f"Keyboard state timeout: {player['dir']} {state(player)}")

    def focus(player):
        matches = []
        def visit(window, _):
            pid, rect = wintypes.DWORD(), wintypes.RECT()
            user.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == player["process"].pid and user.IsWindowVisible(window) and user.GetClientRect(window, ctypes.byref(rect)):
                if rect.right > 600 and rect.bottom > 400:
                    matches.append(window)
            return True
        user.EnumWindows(callback_type(visit), 0)
        if len(matches) != 1:
            raise RuntimeError("Expected one rendered viewport for the selected process")
        window = matches[0]
        user.SetForegroundWindow(window)
        time.sleep(0.3)
        if user.GetForegroundWindow() != window:
            raise RuntimeError("Refusing input to another foreground window")
        return window

    def key(player, vk, label):
        window = focus(player)
        user.keybd_event(vk, 0, 0, 0)
        user.keybd_event(vk, 0, 2, 0)
        time.sleep(0.25)
        if user.GetForegroundWindow() != window:
            raise RuntimeError("Viewport lost foreground during key input")
        actions.append({"action": "os_key", "player": player["name"], "key": label, "pid": player["process"].pid})

    def join_console(player):
        key(player, 0xC0, "Tilde")
        command = f"JoinRoom 127.0.0.1:{port}"
        window = focus(player)
        for ch in command:
            if user.GetForegroundWindow() != window:
                raise RuntimeError("Stopped console typing after focus loss")
            code = user.VkKeyScanW(ch)
            if code == -1 or (code >> 8) & ~1:
                raise RuntimeError("Unsupported console keyboard layout")
            if (code >> 8) & 1:
                user.keybd_event(0x10, 0, 0, 0)
            user.keybd_event(code & 255, 0, 0, 0)
            user.keybd_event(code & 255, 0, 2, 0)
            if (code >> 8) & 1:
                user.keybd_event(0x10, 0, 2, 0)
            time.sleep(0.02)
        actions.append({"action": "os_console_text", "player": player["name"], "command": command})
        key(player, 0x0D, "Console Enter")
        wait(player, lambda s: s.get("members") == 2)
        if state(player).get("console_open"):
            key(player, 0x1B, "Escape Close Console")
        wait(player, lambda s: not s.get("console_open"))

    def checkpoint(player, tag):
        player["seq"] += 1
        directory = player["state_dir"]
        temp = directory / "command.tmp"
        temp.write_text(json.dumps({"seq": player["seq"], "tag": tag}), encoding="utf-8")
        deadline = time.monotonic() + 3
        while True:
            try:
                temp.replace(directory / "command.json")
                break
            except PermissionError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        if tag == "finish":
            return
        deadline = time.monotonic() + (30 if render and tag.startswith("playing") else 10)
        while time.monotonic() < deadline:
            try:
                ack = json.loads((directory / "ack.json").read_text(encoding="utf-8"))
            except (FileNotFoundError, PermissionError, json.JSONDecodeError):
                ack = {}
            if ack.get("seq") == player["seq"]:
                if not ack.get("passed"):
                    raise RuntimeError(f"UE checkpoint failed: {player['name']} {tag}")
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("Checkpoint acknowledgment timeout")
        window = focus(player)
        rect, origin = wintypes.RECT(), wintypes.POINT()
        if not user.GetClientRect(window, ctypes.byref(rect)) or not user.ClientToScreen(window, ctypes.byref(origin)):
            raise ctypes.WinError(ctypes.get_last_error())
        ImageGrab.grab(bbox=(origin.x, origin.y, origin.x + rect.right, origin.y + rect.bottom), all_screens=True).save(directory / f"{tag}.png")
        # state.json is refreshed by UE; a concurrent read may return the empty
        # retry sentinel. Never persist that sentinel as an observed checkpoint.
        observed = wait(player, lambda s: bool(s.get("scene", {}).get("camera", {}).get("manager_valid")), 10)
        snapshots.append({"player": player["name"], "tag": tag, "state": observed})

    def both(tag):
        for p in players:
            checkpoint(p, tag)

    try:
        host = launch(game, "host", 20)
        receiver = launch(client, "receiver", 1100)
        players = [host, receiver]
        for p, target in ((host, 0), (receiver, 1)):
            wait(p, lambda s: s.get("editor") == 0 and s.get("client") == target and s.get("prepared"), 100)
        both("local")
        key(host, 0x75, "F6 HostRoom")
        wait(host, lambda s: s.get("host") and s.get("members") == 1)
        listening = logs(host)
        ports = re.findall(r"IpNetDriver listening on port (\d+)", listening)
        if not ports or int(ports[-1]) != port:
            raise RuntimeError("F6 host bound an unexpected port")
        key(host, 0x74, "F5 Single Member Rejected")
        wait(host, lambda s: "StartRejected Id=1" in logs(host) and s.get("flow") == 0)
        join_console(receiver)
        for p in players:
            wait(p, lambda s: s.get("members") == 2 and s.get("none_ready") and s.get("flow") == 0)
        both("joined")
        rejected = logs(host).count("StartRejected Id=1")
        key(host, 0x74, "F5 Unready Rejected")
        wait(host, lambda s: logs(host).count("StartRejected Id=1") > rejected and s.get("flow") == 0)
        both("unready")
        settlements = []
        for round_index in (1, 2):
            for p in players:
                key(p, 0x0D, "Enter Ready")
            for p in players:
                wait(p, lambda s: s.get("all_ready") and s.get("flow") == 0)
            both(f"ready{round_index}")
            if round_index == 1:
                key(receiver, 0x74, "F5 NonHost Rejected")
                wait(host, lambda s: "StartRejected Id=2" in logs(host) and s.get("flow") == 0)
                both("nonhost")
            key(host, 0x74, "F5 Start")
            for p in players:
                wait(p, lambda s: s.get("flow") == 1)
            both(f"playing{round_index}")
            for p in players:
                wait(p, lambda s: s.get("flow") == 2 and s.get("settled") and s.get("score_round") == s.get("round"), 105)
            fields = ("round", "score_round", "score", "resources", "kills", "result", "base", "base_max", "active_enemies")
            values = [{k: state(p)[k] for k in fields} for p in players]
            if values[0] != values[1]:
                raise RuntimeError(f"Rendered host/client settlement mismatch: {values}")
            settlements.append(values[0])
            both(f"settled{round_index}")
            key(host, 0x1B, "Escape ReturnRoom")
            for p in players:
                wait(p, lambda s: s.get("flow") == 0 and s.get("none_ready") and not s.get("settled"))
            both(f"returned{round_index}")
        for p in players:
            checkpoint(p, "finish")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if all("Test Completed. Result={Success} Name={OSKeyboardTwoRounds}" in logs(p) for p in players):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("Both processes did not finish automation successfully")
        expected = ["local", "joined", "unready", "ready1", "nonhost", "playing1", "settled1", "returned1", "ready2", "playing2", "settled2", "returned2"]
        verified = []
        for p in players:
            if p["process"].poll() is not None or "Result={Fail}" in logs(p):
                raise RuntimeError("Process exited or failed before final evidence")
            result = json.loads((p["state_dir"] / "result.json").read_text(encoding="utf-8"))
            checks = result.get("checkpoints", [])
            if not result.get("passed") or [c.get("tag") for c in checks] != expected or not all(c.get("passed") for c in checks):
                raise RuntimeError("Missing twelve successful ordered checkpoints")
            verified.append({c["tag"]: c for c in checks})
        settlements = []
        for tag in ("settled1", "settled2"):
            values = [{k: v[tag][k] for k in fields} for v in verified]
            if values[0] != values[1] or any(not v[tag].get("settled") or v[tag]["score_round"] != v[tag]["round"] for v in verified):
                raise RuntimeError("Final confirmed settlement checkpoints differ across host and client")
            settlements.append(values[0])
        from scene_visual_gate import validate_camera, validate_report
        # Current snapshots as well as all twelve authoritative checkpoints must
        # retain the fixed runtime camera through local/join/settlement/return.
        for snapshot in snapshots:
            validate_camera(snapshot["state"].get("scene", {}))
        scene_reports = []
        for player, checkpoints in zip(players, verified):
            for tag in expected:
                validate_camera(checkpoints[tag].get("scene", {}))
                if not checkpoints[tag]["scene"].get("meshes"):
                    raise RuntimeError(f"Missing final mesh checkpoint: {player['name']} {tag}")
            if render:
                for tag in ("playing1", "playing2"):
                    path = player["state_dir"] / "scene" / tag / "report.json"
                    if Path(checkpoints[tag].get("scene_report", "")).resolve() != path.resolve():
                        raise RuntimeError("Missing fresh checkpoint-owned scene report")
                    scene_reports.append(validate_report(path, require_enemy=False))
        result = {"passed": True, "packaged_game_client": True, "rendered_both": True, "automated_os_input": True,
                  "scene_visual_validated": bool(scene_reports) and all(r["scene_visual_validated"] for r in scene_reports),
                  "scene_reports": scene_reports,
                  "visual_scope": "Two playing rounds: static official attributed pixels; settlement/return camera and mesh checkpoints" if render else "HUD/keyboard plus camera/mesh checkpoints; no pixel acceptance",
                  "human_ui_validated": False, "same_machine_not_cross_device": True, "fresh_cook": cook,
                  "rounds": 2, "settlements": settlements, "settlement_source": "final verified checkpoints", "port": port, "snapshots": snapshots}
        (output / "os-input-actions.json").write_text(json.dumps(actions, ensure_ascii=False, indent=2), encoding="utf-8")
        path = output / "keyboard_result.json"
        path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(path)
        print("rendered_two_rounds_passed=True")
    finally:
        kernel.CloseHandle(job)
        cleanup_errors = []
        for process in processes:
            try:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=15)
            except OSError as error:
                cleanup_errors.append(str(error))
            except subprocess.TimeoutExpired as error:
                cleanup_errors.append(str(error))
        for stream in streams:
            stream.close()
        if cleanup_errors:
            (output / "keyboard_result.json").unlink(missing_ok=True)
            raise RuntimeError("Keyboard process cleanup failed: " + "; ".join(cleanup_errors))


def collect_room_settlements(text, path, rounds, members):
    lines = re.findall(r"\[AkUGCRoomTest\] Settlement .*", text)
    values = [dict(re.findall(r"\b(Round|Score|Resources|Kills|Base|Members)=(\d+)", line)) for line in lines]
    fields = {"Round", "Score", "Resources", "Kills", "Base", "Members"}
    if len(values) != rounds or any(set(value) != fields or int(value["Members"]) != members for value in values):
        raise RuntimeError(f"Incomplete room settlements: {path}: {values}")
    if any(int(after["Round"]) <= int(before["Round"]) for before, after in zip(values, values[1:])):
        raise RuntimeError(f"Room settlement Round did not increase: {path}: {values}")
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--seeds", type=int, default=1)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--client", type=Path, help="Archived Client executable (default DS mode only)")
    parser.add_argument("--server", type=Path, help="Archived Server executable (default DS mode only)")
    parser.add_argument("--members", type=int, choices=(2, 4), default=4)
    parser.add_argument("--cook", action="store_true")
    parser.add_argument("--listen", action="store_true")
    parser.add_argument("--reconnect", action="store_true")
    parser.add_argument("--recovery", choices=("leave", "crash", "timeout"))
    parser.add_argument("--packaged-recovery", action="store_true")
    parser.add_argument("--host-entry-ui", action="store_true")
    parser.add_argument("--keyboard-rounds", action="store_true")
    parser.add_argument("--render", action="store_true", help="Keyboard rounds require native 3D attributed pixels")
    parser.add_argument("--output", type=Path, help="Fresh isolated output directory")
    options = parser.parse_args()
    default_ds = not (options.listen or options.reconnect or options.recovery or options.packaged_recovery or options.host_entry_ui or options.keyboard_rounds)
    if not default_ds and options.members != 4:
        parser.error("--members other than 4 is supported only in default DS mode")
    if not default_ds and (options.client is not None or options.server is not None):
        parser.error("--client and --server are supported only in default DS mode")
    if options.keyboard_rounds and (options.recovery or options.listen or options.reconnect or options.packaged_recovery or options.host_entry_ui or options.seeds != 1 or options.rounds != 1):
        parser.error("Keyboard two-round flow runs separately from other room modes")
    if options.packaged_recovery and not options.recovery:
        parser.error("--packaged-recovery requires --recovery")
    if options.host_entry_ui and (not options.packaged_recovery or options.recovery != "leave"):
        parser.error("--host-entry-ui requires packaged leave recovery")
    if options.seeds < 1 or options.rounds < 1:
        parser.error("--seeds and --rounds must be positive")
    if options.listen and options.reconnect:
        parser.error("Listen and reconnect scenarios run separately")
    root = Path(__file__).resolve().parents[2]
    if options.render and not options.keyboard_rounds:
        parser.error("--render scene attribution is supported only with --keyboard-rounds")
    output = options.output.resolve() if options.output else root / "Saved/RoomVerification" / uuid.uuid4().hex
    output.mkdir(parents=True, exist_ok=False)
    if options.keyboard_rounds:
        verify_keyboard_rounds(options.engine, root, output, options.cook, options.render)
        return
    if options.recovery:
        if options.listen or options.reconnect or options.cook and not options.packaged_recovery or options.seeds != 1 or options.rounds != 1:
            parser.error("Recovery runs separately from room round modes; cook requires packaged recovery")
        verify_recovery(options.engine, root, output, options.recovery, options.packaged_recovery, options.cook, options.host_entry_ui)
        return
    editor = options.engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    project = root / "AkUGCPlatform.uproject"
    if options.cook:
        uat = options.engine / "Engine/Build/BatchFiles/RunUAT.bat"
        for mode, target in (("Client", "AkUGCPlatformClient"), ("Server", "AkUGCPlatformServer")):
            args = [str(uat), "BuildCookRun", f"-project={project}", "-noP4", "-unattended", "-utf8output",
                    "-platform=Win64", f"-target={target}", "-build", "-cook", "-stage", "-pak", "-package"]
            args += (["-client", "-noserver", "-clientconfig=Development"] if mode == "Client" else
                     ["-server", "-noclient", "-serverplatform=Win64", "-serverconfig=Development"])
            run_checked(args, output / f"cook_{mode.lower()}.log", 3600)
    client = options.client.resolve() if options.client else root / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
    server = options.server.resolve() if options.server else root / "Saved/StagedBuilds/WindowsServer/AkUGCPlatform/Binaries/Win64/AkUGCPlatformServer.exe"
    for explicit, executable in ((options.client, client), (options.server, server)):
        if explicit is not None and not executable.is_file():
            raise RuntimeError(f"Missing executable: {executable}")
    results = []
    for seed in range(options.seeds):
        directory = output / f"{'listen' if options.listen else 'reconnect' if options.reconnect else 'ds'}_seed_{seed}"
        directory.mkdir(parents=True, exist_ok=True)
        pack, key = directory / "pack.json", directory / "key.txt"
        run_checked([str(editor), str(project), "-run=AkUGCPackTool", "-NetworkVisualFixture", "-RoomFixture",
                     f"-Seed={seed}", f"-Output={pack}", f"-PublicKey={key}", "-unattended", "-nop4", "-nullrhi"], directory / "fixture.log", 180)
        processes, handles = [], []
        port = (18077 if options.listen else 18177 if options.reconnect else 17977) + seed

        def launch(executable, args, name):
            log = directory / f"{name}.log"
            log.unlink(missing_ok=True)
            handle = (directory / f"{name}.stdout.log").open("w", encoding="utf-8")
            handles.append(handle)
            explicit = options.server if name == "host" else options.client
            process = subprocess.Popen([str(executable), *args, "-unattended", "-nop4", "-nosplash", "-nullrhi", "-stdout", f"-abslog={log}"],
                                       cwd=executable.parent if explicit else root, stdout=handle, stderr=subprocess.STDOUT)
            processes.append(process)
            return process, log

        test_args = ["-AkUGCRoomTest", f"-RoomExpectedMembers={2 if options.reconnect else options.members}", f"-RoomTestRounds={options.rounds}",
                     "-ExecCmds=Automation RunTests AkUGC.Runtime.Room.RealNetwork"]
        try:
            host_args = ["/Game/Maps/Phase0?listen?room=1" if options.listen else "/Game/Maps/Phase0",
                         "-AkUGCRoom", f"-port={port}", f"-LogicPack={pack}", f"-LogicPackKey={key.read_text(encoding='utf-8-sig').strip()}"]
            ds, ds_log = launch(editor if options.listen else server,
                                ([str(project), "-game"] if options.listen else []) + host_args + (test_args if options.listen else []), "host")
            wait_for(ds, ds_log, "listening on port", 60)
            players = []
            if options.listen:
                players.append((ds, ds_log))
            initial_count = (2 if options.reconnect else options.members) - int(options.listen)
            for index in range(initial_count):
                player = launch(client, [f"127.0.0.1:{port}", *test_args,
                    *(["-RoomExpectedMembers=2"] if options.reconnect else [])], f"client{index+1}")
                wait_for(*player, "[AkUGCRoomTest] Joined", 35)
                players.append(player)
            wait_for(ds, ds_log, "[AkUGCRoom] Started", 50)
            departing = None
            if options.reconnect:
                wait_for(*players[0], "[AkUGCRoomTest] EnemyObserved", 45)
                late = launch(client, [f"127.0.0.1:{port}", *test_args], "late_join")
                wait_for(*late, "[AkUGCRoomTest] Joined", 35)
                players.append(late)
                departing = launch(client, [f"127.0.0.1:{port}", *test_args, "-RoomTestDisconnect"], "departing")
                wait_for(*departing, "[AkUGCRoomTest] Joined", 35)
            if options.members == 4:
                fifth, fifth_log = launch(client, [f"127.0.0.1:{port}"], "rejected_fifth")
                wait_for(fifth, fifth_log, "RoomFull", 35)
            if departing:
                left = wait_for(*departing, "[AkUGCRoomTest] LeftForReconnect", 35)
                old_id = re.search(r"LeftForReconnect Id=(\d+)", left).group(1)
                wait_for(ds, ds_log, f"Left Id={old_id}", 20)
                rejoined = launch(client, [f"127.0.0.1:{port}", *test_args], "reconnected")
                joined = wait_for(*rejoined, "[AkUGCRoomTest] Joined", 35)
                new_id = re.search(r"Joined Id=(\d+)", joined).group(1)
                if new_id == old_id:
                    raise RuntimeError("Reconnected client unexpectedly reused old identity.")
                players.append(rejoined)
            evidence, settlements, lua_startup = [], {}, {}
            for process, log in players:
                text = wait_for(process, log, "Test Completed. Result={Success} Name={RealNetwork}", 170)
                if "Result={Fail}" in text:
                    raise RuntimeError(f"Room test failed: {log}")
                matches = re.findall(r"\[AkUGCRoomTest\] Passed .*", text)
                if not matches:
                    raise RuntimeError(f"Missing room Passed evidence: {log}")
                evidence.append(matches[-1])
                if default_ds:
                    settlements[log.stem] = collect_room_settlements(text, log, options.rounds, options.members)
                    lua_startup[log.stem] = verify_lua_startup(text, log)
                    final = dict(re.findall(r"\b(Round|Score|Resources|Kills|Base|Members)=(\d+)", matches[-1]))
                    observed_rounds = re.search(r"\bRounds=(\d+)", matches[-1])
                    if final != settlements[log.stem][-1] or not observed_rounds or int(observed_rounds.group(1)) != options.rounds:
                        raise RuntimeError(f"Final signature differs from last settlement: {log}")
            signatures = [dict(re.findall(r"(Round|Score|Resources|Kills|Base)=(\d+)", line)) for line in evidence]
            if any(value != signatures[0] for value in signatures):
                raise RuntimeError(f"Room settlement mismatch: {signatures}")
            case = {"seed": seed, "members": options.members, "rounds": options.rounds, "fifth_rejected": options.members == 4,
                    "late_join_and_reconnect": options.reconnect,
                    "settlement": signatures[0], "client_evidence": evidence}
            if default_ds:
                if any(value != next(iter(settlements.values())) for value in settlements.values()):
                    raise RuntimeError(f"Per-round room settlement mismatch: {settlements}")
                host_text = read_log(ds_log)
                if ds.poll() is not None or "Result={Fail}" in host_text or "Authority Logic Pack load failed" in host_text:
                    raise RuntimeError(f"DS host failed before final evidence: {ds_log}")
                lua_startup["host"] = verify_lua_startup(host_text, ds_log)
                starts = re.findall(r"\[AkUGCRoom\] Started Members=(\d+) Round=(\d+)", host_text)
                reference = next(iter(settlements.values()))
                if host_text.count("[AkUGCRoom] Started") != options.rounds or starts != [(value["Members"], value["Round"]) for value in reference]:
                    raise RuntimeError(f"Host Started count/rounds differ from settlements: {ds_log}: {starts}")
                # ReturnRoomToPreparation emits no return log. A later Started proves
                # the intervening return/readiness cycle; the final round is not returned.
                case.update({"settlements": settlements, "lua_startup": lua_startup,
                             "host_evidence": re.findall(r"\[AkUGCRoom\].*", host_text),
                             "return_and_restart_cycles": max(0, options.rounds - 1),
                             "first_round_return_and_restart": options.rounds > 1,
                             "final_round_return_verified": False,
                             "return_evidence": "Increasing host Started and matching client Settlement rounds; no explicit return log"})
            results.append(case)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=15)
            for handle in handles:
                handle.close()
    result = {"mode": "listen" if options.listen else "dedicated_server", "cases": results,
              "sources": {"client": str(client), "server": str(server), "fixture_editor": str(editor)},
              "fresh_cook": options.cook, "rendering": "NullRHI network/room verification"}
    result_path = output / ("listen_result.json" if options.listen else "reconnect_result.json" if options.reconnect else "ds_result.json")
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(result_path)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
