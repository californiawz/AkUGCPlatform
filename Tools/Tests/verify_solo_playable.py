import json
from pathlib import Path
import subprocess


root = Path(__file__).resolve().parents[2]
output = root / "Saved" / "SoloVerification"
output.mkdir(parents=True, exist_ok=True)
log = output / "playable.log"
capture = output / "settlement.png"
for path in (log, capture, output / "result.json"):
    path.unlink(missing_ok=True)
executable = root / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
arguments = [str(executable), "/Game/Maps/Phase0", "-AkUGCSoloSmoke", "-windowed", "-ResX=1280", "-ResY=720",
             "-ExecCmds=Automation RunTests AkUGC.Runtime.Solo.PlayableSmoke",
             "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-nosplash", "-stdout",
             f"-abslog={log}", f"-AkUGCHUDCapture={capture}"]
with (output / "stdout.log").open("w", encoding="utf-8") as stream:
    process = subprocess.Popen(arguments, cwd=root, stdout=stream, stderr=subprocess.STDOUT)
    try:
        code = process.wait(timeout=200)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=15)
text = log.read_text(encoding="utf-8", errors="replace")
if code or "[AkUGCSoloSmoke] Passed" not in text or "Result={Fail}" in text or not capture.exists():
    raise RuntimeError(f"Playable solo verification failed: {log}\n{text[-5000:]}")
result = {"standalone": True, "bot_preparation": True, "player_controller_start_return": True,
          "rendered_settlement_capture": str(capture),
          "events": [line for line in text.splitlines() if "[AkUGCSoloSmoke]" in line]}
(output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(result, ensure_ascii=False, indent=2))
