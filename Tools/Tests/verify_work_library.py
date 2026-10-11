import argparse
import json
from pathlib import Path
import uuid

from verify_network_visuals import run_checked


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--cook", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = root / "Saved/WorkLibraryVerification" / uuid.uuid4().hex
    output.mkdir(parents=True)
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
    metadata = output / "published.json"
    run_checked([str(editor), str(project), "-run=AkUGCPackTool", "-PublishWork", f"-Library={output / 'Author'}",
                 f"-Output={metadata}", "-unattended", "-nop4", "-nullrhi"], output / "publish.log", 180)
    published = json.loads(metadata.read_text(encoding="utf-8-sig"))
    share = Path(published["sharePath"])
    pack = json.loads(share.read_text(encoding="utf-8-sig"))
    # Corrupt the signed document, not the container syntax.
    document_key = next(key for key in pack if key.lower() == "document")
    manifest_key = next(key for key in pack[document_key] if key.lower() == "manifest")
    name_key = next(key for key in pack[document_key][manifest_key] if key.lower() == "displayname")
    pack[document_key][manifest_key][name_key] = "Modified after publication"
    bad_share = output / "tampered.akugc"
    bad_share.write_text(json.dumps(pack), encoding="utf-8")
    executable = root / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
    evidence = []
    for index in range(2):
        log = output / f"play_{index}.log"
        args = [str(executable), "/Game/Maps/Phase0", "-AkUGCWorkPlay", f"-WorkId={published['releaseId']}",
                f"-WorkLibrary={output / 'Receiver'}", "-ExecCmds=Automation RunTests AkUGC.Runtime.WorkLibrary.SharedPlayable",
                "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-nosplash", "-nullrhi", "-stdout", f"-abslog={log}"]
        if index == 0:
            args += [f"-WorkShare={share}", f"-WorkTrustedKey={published['publicKey']}", f"-WorkBadShare={bad_share}"]
        else:
            args.remove("-nullrhi")
            args += ["-windowed", "-ResX=1280", "-ResY=720", f"-AkUGCHUDCapture={output / 'settlement.png'}"]
        run_checked(args, output / f"play_{index}.stdout.log", 190)
        raw = log.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("gb18030", errors="replace")
        if "[AkUGCWorkPlay] Passed" not in text or "Result={Fail}" in text or "Test Completed. Result={Success} Name={SharedPlayable}" not in text:
            raise RuntimeError(f"Shared work play failed: {log}")
        if index == 1 and not (output / "settlement.png").is_file():
            raise RuntimeError("Rendered shared work did not produce settlement capture.")
        evidence += [line for line in text.splitlines() if "[AkUGCWorkPlay]" in line or "[AkUGCWorkLibrary] Import=" in line]
    result = {"release_id": published["releaseId"], "independent_libraries": True,
              "untrusted_and_tampered_rejected": True, "receiver_restarted_and_replayed": True,
              "fresh_cook": options.cook, "rendering": "Restarted receiver rendered actual standalone match",
              "settlement_capture": str(output / "settlement.png"), "evidence": evidence}
    result_path = output / "result.json"
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(result_path)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
