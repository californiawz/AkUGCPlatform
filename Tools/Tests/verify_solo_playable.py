import argparse
import json
from pathlib import Path
import subprocess
import uuid


parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, help="Fresh output directory; existing evidence is never overwritten")
parser.add_argument("--render", action="store_true", help="Require new native viewport attributed 3D evidence")
parser.add_argument("--executable", type=Path, help="Verify a specific archived package instead of the default staged client")
options = parser.parse_args()
root = Path(__file__).resolve().parents[2]
output = options.output.resolve() if options.output else root / "Saved/SoloVerification" / uuid.uuid4().hex
output.mkdir(parents=True, exist_ok=False)
log = output / "playable.log"
capture = output / "settlement.png"
executable = (options.executable.resolve() if options.executable else
              root / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe")
if not executable.is_file():
    raise FileNotFoundError(f"Packaged client not found: {executable}")
arguments = [str(executable), "/Game/Maps/Phase0", "-AkUGCSoloSmoke", "-windowed", "-ResX=1280", "-ResY=720",
             "-ExecCmds=Automation RunTests AkUGC.Runtime.Solo.PlayableSmoke",
             "-unattended", "-nop4", "-nosplash", "-stdout",
             f"-abslog={log}", f"-AkUGCHUDCapture={capture}"]
if options.render:
    from scene_visual_gate import validate_report
    arguments.append(f"-AkUGCSceneVisual={output / 'scene'}")


def close_game_window(process):
    import ctypes
    from ctypes import wintypes
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    windows = []

    @callback_type
    def visit(window, _):
        pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(pid))
        if pid.value == process.pid and user.IsWindowVisible(window):
            windows.append(window)
        return True

    user.EnumWindows(visit, 0)
    if len(windows) != 1:
        raise RuntimeError(f"Expected one visible game window for PID {process.pid}, got {len(windows)}")
    if not user.PostMessageW(windows[0], 0x0010, 0, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    return {"action": "WM_CLOSE", "pid": process.pid, "hwnd": int(windows[0])}


with (output / "stdout.log").open("w", encoding="utf-8") as stream:
    process = subprocess.Popen(arguments, cwd=executable.parent if options.executable else root,
                               stdout=stream, stderr=subprocess.STDOUT)
    try:
        import time
        deadline = time.monotonic() + 180
        while process.poll() is None:
            current = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
            if "Test Completed. Result=" in current and "Path={AkUGC.Runtime.Solo.PlayableSmoke}" in current:
                close_action = close_game_window(process)
                (output / "close_action.json").write_text(json.dumps(close_action, indent=2), encoding="utf-8")
                break
            if time.monotonic() >= deadline:
                raise TimeoutError(f"Solo smoke timed out: {log}")
            time.sleep(0.2)
        code = process.wait(timeout=20)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=15)
text = log.read_text(encoding="utf-8", errors="replace")
if code or "[AkUGCSoloSmoke] Passed" not in text or "Result={Fail}" in text or not capture.exists():
    raise RuntimeError(f"Playable solo verification failed: {log}\n{text[-5000:]}")
lua_events = [line for line in text.splitlines() if "[AkUGCPlatform]" in line or "LogAkLua:" in line]
lua_checks = {
    "packaged_initialize": any("[AkUGCPlatform] Lua VM initialized" in line and "editor=false" in line
                               for line in lua_events),
    "bootstrapped": any("[Subsystem] Lua bootstrapped" in line for line in lua_events),
    "gameplay_started": any("[AkUGCPlatform] gameplay started" in line for line in lua_events),
    "shutdown": any("[AkUGCPlatform] Lua VM shutdown" in line for line in lua_events),
}
lua_errors = [line for line in text.splitlines()
              if "[Loader] module not found:" in line or "tearing down" in line.lower()
              or "did not return a table" in line
              or (("LogAkLua:" in line or "LogSlua:" in line) and "Error:" in line)
              or "stack traceback:" in line]
if not all(lua_checks.values()) or lua_errors:
    raise RuntimeError(f"Packaged Lua verification failed: {log}\nChecks: {lua_checks}\nErrors: {lua_errors}\n"
                       + "\n".join(lua_events))
result = {"standalone": True, "bot_preparation": True, "player_controller_start_return": True,
          "executable": str(executable), "exit_code": code,
          "lua_validated": True, "lua_checks": lua_checks, "lua_events": lua_events,
          "rendered_settlement_capture": str(capture),
          "events": [line for line in text.splitlines() if "[AkUGCSoloSmoke]" in line]}
result["scene_visual_validated"] = False
if options.render:
    result["scene_visual"] = validate_report(output / "scene/solo/report.json")
    from scene_visual_gate import validate_camera
    result["scene_checkpoints"] = {}
    for phase in ("settlement", "returned"):
        scene = json.loads((output / f"scene/solo/{phase}.json").read_text(encoding="utf-8-sig"))
        validate_camera(scene)
        if not scene.get("meshes"):
            raise RuntimeError(f"Missing {phase} mesh checkpoint")
        result["scene_checkpoints"][phase] = scene
    result["scene_visual_validated"] = result["scene_visual"]["scene_visual_validated"]
(output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(result, ensure_ascii=False, indent=2))
