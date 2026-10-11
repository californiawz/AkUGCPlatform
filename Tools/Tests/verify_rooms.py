import argparse
import json
from pathlib import Path
import re
import subprocess
import time

from verify_network_visuals import read_log, run_checked, wait_for


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--seeds", type=int, default=1)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--cook", action="store_true")
    parser.add_argument("--listen", action="store_true")
    parser.add_argument("--reconnect", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = root / "Saved/RoomVerification"
    output.mkdir(parents=True, exist_ok=True)
    (output / "result.json").unlink(missing_ok=True)
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
    client = root / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
    server = root / "Saved/StagedBuilds/WindowsServer/AkUGCPlatform/Binaries/Win64/AkUGCPlatformServer.exe"
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
            process = subprocess.Popen([str(executable), *args, "-unattended", "-nop4", "-nosplash", "-nullrhi", "-stdout", f"-abslog={log}"],
                                       cwd=root, stdout=handle, stderr=subprocess.STDOUT)
            processes.append(process)
            return process, log

        test_args = ["-AkUGCRoomTest", f"-RoomExpectedMembers={2 if options.reconnect else 4}", f"-RoomTestRounds={options.rounds}",
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
            initial_count = (2 if options.reconnect else 4) - int(options.listen)
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
            evidence = []
            for process, log in players:
                text = wait_for(process, log, "Test Completed. Result={Success} Name={RealNetwork}", 170)
                if "Result={Fail}" in text:
                    raise RuntimeError(f"Room test failed: {log}")
                matches = re.findall(r"\[AkUGCRoomTest\] Passed .*", text)
                evidence.append(matches[-1])
            signatures = [dict(re.findall(r"(Round|Score|Resources|Kills|Base)=(\d+)", line)) for line in evidence]
            if any(value != signatures[0] for value in signatures):
                raise RuntimeError(f"Room settlement mismatch: {signatures}")
            results.append({"seed": seed, "members": 4, "rounds": options.rounds, "fifth_rejected": True,
                            "late_join_and_reconnect": options.reconnect,
                            "settlement": signatures[0], "client_evidence": evidence})
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
              "fresh_cook": options.cook, "rendering": "NullRHI network/room verification"}
    result_path = output / ("listen_result.json" if options.listen else "reconnect_result.json" if options.reconnect else "ds_result.json")
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
