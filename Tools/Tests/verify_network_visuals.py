import argparse
import json
import pathlib
import re
import socket
import subprocess
import time
import uuid

from unreal_automation_verdict import require_automation_success


def read_log(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def verify_lua_startup(text, path):
    events = [line for line in text.splitlines() if "[AkUGCPlatform]" in line or "LogAkLua:" in line or "LogSlua:" in line]
    checks = {
        "packaged_initialize": any("[AkUGCPlatform] Lua VM initialized" in line and "editor=false" in line for line in events),
        "bootstrapped": any("[Subsystem] Lua bootstrapped" in line for line in events),
        "gameplay_started": any("[AkUGCPlatform] gameplay started" in line for line in events),
        "map_load_complete": any("[AkUGCPlatform] map load complete" in line for line in events),
    }
    errors = [line for line in text.splitlines()
              if re.search(r"Log(?:AkLua|Slua):\s*Error:", line, re.IGNORECASE)
              or "module not found" in line.lower() or "tearing down" in line.lower()
              or "did not return a table" in line.lower() or "stack traceback:" in line.lower()
              or re.search(r"\bentry\b.*\b(?:fail(?:ed|ure)?|uninitialized)\b", line, re.IGNORECASE)]
    if not all(checks.values()) or errors:
        raise RuntimeError(f"Packaged Lua startup failed: {path}\nChecks: {checks}\nErrors: {errors}\n" + "\n".join(events))
    return {"validated": True, "checks": checks, "events": events}


def wait_for(process, path, marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = read_log(path)
        if "Authority Logic Pack load failed" in text or "Result={Fail}" in text:
            raise RuntimeError(f"Verification failed before {marker}: {path}\n{text[-4000:]}")
        if marker in text:
            return text
        if process.poll() is not None:
            raise RuntimeError(f"Process exited before {marker}: {path}\n{text[-3000:]}")
        time.sleep(0.25)
    raise RuntimeError(f"Timed out waiting for {marker}: {path}")


def run_checked(args, path, timeout):
    with path.open("w", encoding="utf-8") as output:
        result = subprocess.run(args, stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Command returned {result.returncode}: {path}\n{read_log(path)[-5000:]}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, type=pathlib.Path)
    parser.add_argument("--cook", action="store_true")
    parser.add_argument("--render", action="store_true")
    parser.add_argument("--port", type=int, default=17877)
    parser.add_argument("--client", type=pathlib.Path, help="Archived Client executable (defaults to staged build)")
    parser.add_argument("--server", type=pathlib.Path, help="Archived Server executable (defaults to staged build)")
    parser.add_argument("--output", type=pathlib.Path, help="Fresh output directory (defaults to isolated UUID)")
    options = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    project = root / "AkUGCPlatform.uproject"
    output = options.output.resolve() if options.output else root / "Saved/NetworkVisualVerification" / uuid.uuid4().hex
    output.mkdir(parents=True, exist_ok=False)
    if options.render:
        from scene_visual_gate import validate_report
    editor = options.engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    uat = options.engine / "Engine/Build/BatchFiles/RunUAT.bat"
    if options.cook:
        for mode, target in (("Client", "AkUGCPlatformClient"), ("Server", "AkUGCPlatformServer")):
            args = [str(uat), "BuildCookRun", f"-project={project}", "-noP4", "-unattended",
                    "-utf8output", "-platform=Win64", f"-target={target}", "-build", "-cook",
                    "-stage", "-pak", "-package", "-noisocreate"]
            args += (["-client", "-noserver", "-clientconfig=Development"] if mode == "Client"
                     else ["-server", "-noclient", "-serverplatform=Win64", "-serverconfig=Development"])
            run_checked(args, output / f"cook_{mode.lower()}.log", 3600)
    pack = output / "fixture.json"
    key = output / "public_key.txt"
    run_checked([str(editor), str(project), "-run=AkUGCPackTool", "-NetworkVisualFixture",
                 f"-Output={pack}", f"-PublicKey={key}", "-unattended", "-nop4", "-nosplash", "-nullrhi"],
                output / "fixture.log", 180)
    key_value = key.read_text(encoding="utf-8-sig").strip()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", key_value):
        raise RuntimeError("Fixture public key is invalid.")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", options.port))
    client = options.client.resolve() if options.client else root / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
    server = options.server.resolve() if options.server else root / "Saved/StagedBuilds/WindowsServer/AkUGCPlatform/Binaries/Win64/AkUGCPlatformServer.exe"
    for executable in (client, server):
        if not executable.is_file():
            raise RuntimeError(f"Missing executable: {executable}")
    common = ["-AkUGCNetworkVisual", "-unattended", "-nop4", "-nosplash", "-nullrhi", "-stdout"]
    processes = []
    handles = []
    started = time.monotonic()

    def launch(executable, arguments, name):
        log = output / f"{name}.log"
        handle = (output / f"{name}.stdout.log").open("w", encoding="utf-8")
        handles.append(handle)
        launch_options = common
        if options.render and name != "server":
            launch_options = [arg for arg in common if arg != "-nullrhi"]
            capture = output / f"{name}_hud.png"
            launch_options += ["-windowed", "-ResX=1280", "-ResY=720", f"-AkUGCHUDCapture={capture}",
                               f"-AkUGCSceneVisual={output / (name + '_scene')}"]
        explicit = options.server if name == "server" else options.client
        process = subprocess.Popen([str(executable), *arguments, *launch_options, f"-abslog={log}"],
                                   cwd=executable.parent if explicit else root, stdout=handle, stderr=subprocess.STDOUT)
        processes.append(process)
        return process, log

    try:
        ds, ds_log = launch(server, ["/Game/Maps/Phase0", f"-port={options.port}",
            f"-LogicPack={pack}", f"-LogicPackKey={key_value}",
            "-ExecCmds=Automation RunTests AkUGC.Runtime.Network.ServerVisualBaseline"], "server")
        wait_for(ds, ds_log, "listening on port", 60)
        first, first_log = launch(client, [f"127.0.0.1:{options.port}",
            "-ExecCmds=Automation RunTests AkUGC.Runtime.Network.ClientVisualReplication",
            "-TestExit=Automation Test Queue Empty"], "client1")
        wait_for(first, first_log, "[AkUGCNetworkVisual] ObservedEnemy", 70)
        joined_at = time.monotonic() - started
        second, second_log = launch(client, [f"127.0.0.1:{options.port}",
            "-ExecCmds=Automation RunTests AkUGC.Runtime.Network.ClientVisualReplication",
            "-TestExit=Automation Test Queue Empty"], "client2_late")
        wait_for(second, second_log, "[AkUGCNetworkVisual] ObservedEnemy", 35)
        results = {"sources": {"client": str(client), "server": str(server), "fixture_editor": str(editor)}, "lua_startup": {}}
        for name, process, log in (("client1", first, first_log), ("client2_late", second, second_log)):
            code = process.wait(timeout=110)
            text = read_log(log)
            require_automation_success(
                read_log(log.with_suffix(".stdout.log")),
                expected_selector="AkUGC.Runtime.Network.ClientVisualReplication",
                expected_paths=("AkUGC.Runtime.Network.ClientVisualReplication",),
                expected_total=1,
                require_queue_empty=True,
                require_test_exit=True,
                return_code=code,
            )
            if "[AkUGCNetworkVisual] ClientPassed" not in text:
                raise RuntimeError(f"{name} failed, code={code}: {log}\n{text[-4000:]}")
            results[name] = re.findall(r"\[AkUGCNetworkVisual\].*", text)
            results["lua_startup"][name] = verify_lua_startup(text, log)
        text = wait_for(ds, ds_log, "...Automation Test Queue Empty 1 tests performed.", 15)
        require_automation_success(
            read_log(ds_log.with_suffix(".stdout.log")),
            expected_selector="AkUGC.Runtime.Network.ServerVisualBaseline",
            expected_paths=("AkUGC.Runtime.Network.ServerVisualBaseline",),
            expected_total=1,
            require_queue_empty=True,
        )
        if ds.poll() is not None or "[AkUGCNetworkVisual] ServerPassed" not in text:
            raise RuntimeError(f"Server verification failed: {ds_log}")
        results["server"] = re.findall(r"\[AkUGCNetworkVisual\].*", text)
        results["lua_startup"]["server"] = verify_lua_startup(text, ds_log)
        settlements = []
        for name in ("client1", "client2_late", "server"):
            terminal = next(line for line in results[name] if "Passed" in line)
            fields = dict(re.findall(r"(Score|Resources|Kills|Base|Settled)=(\d+)", terminal))
            if fields != {"Score": "500", "Resources": "321", "Kills": "0", "Base": "993", "Settled": "1"}:
                raise RuntimeError(f"Invalid replicated settlement for {name}: {fields}")
            settlements.append(fields)
        results["settlement"] = settlements[0]
        results["late_join_seconds"] = round(joined_at, 2)
        results["fresh_build_cook_stage_package"] = options.cook
        results["scene_visual_validated"] = False
        results["rendering"] = "Native viewport official static-object attribution" if options.render else "NullRHI object/replication verification; not screenshot validation"
        if options.render:
            captures = [output / f"{name}_hud.png" for name in ("client1", "client2_late")]
            if not all(path.exists() and path.stat().st_size > 0 for path in captures):
                raise RuntimeError("Rendered HUD screenshots were not produced.")
            results["hud_captures"] = [str(path) for path in captures]
            results["scene_reports"] = [validate_report(output / f"{name}_scene/network/report.json")
                                         for name in ("client1", "client2_late")]
            from scene_visual_gate import validate_camera
            results["settlement_scenes"] = []
            for name in ("client1", "client2_late"):
                scene = json.loads((output / f"{name}_scene/network/settlement.json").read_text(encoding="utf-8-sig"))
                validate_camera(scene)
                if not scene.get("meshes"):
                    raise RuntimeError(f"Missing final settlement meshes for {name}")
                results["settlement_scenes"].append(scene)
            results["scene_visual_validated"] = all(r["scene_visual_validated"] for r in results["scene_reports"])
        (output / "result.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(results, ensure_ascii=False, indent=2))
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


if __name__ == "__main__":
    main()
