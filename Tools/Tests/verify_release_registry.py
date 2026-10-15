import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

from unreal_automation_verdict import require_automation_success


def run_checked(arguments, log, timeout, cwd=None, env=None):
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(
            [str(value) for value in arguments],
            cwd=cwd,
            env=env,
            stdout=stream,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    if result.returncode:
        text = log.read_text(encoding="utf-8", errors="replace")
        raise RuntimeError(f"Command returned {result.returncode}: {log}\n{text[-5000:]}")


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def request(base, method, path, token=None, body=None, headers=None, timeout=120):
    values = dict(headers or {})
    if token:
        values["Authorization"] = "Bearer " + token
    call = urllib.request.Request(
        base + path,
        data=body,
        headers=values,
        method=method,
    )
    try:
        with urllib.request.urlopen(call, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def wait_for_health(base, process, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Release Registry exited before health check")
        try:
            status, _, _ = request(base, "GET", "/healthz", timeout=2)
            if status == 200:
                return
        except (OSError, TimeoutError):
            pass
        time.sleep(0.2)
    raise TimeoutError("Release Registry health check timed out")


def json_body(value):
    return json.dumps(value, separators=(",", ":")).encode("utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument(
        "--postgres-bin",
        type=Path,
        default=Path("C:/Program Files/PostgreSQL/16/bin"),
    )
    parser.add_argument("--client", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--skip-play", action="store_true")
    options = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    output = (
        options.output.resolve()
        if options.output
        else root / "Saved/ReleaseRegistryVerification" / uuid.uuid4().hex
    )
    output.mkdir(parents=True, exist_ok=False)
    postgres_bin = options.postgres_bin.resolve()
    for name in ("initdb.exe", "pg_ctl.exe"):
        if not (postgres_bin / name).is_file():
            raise FileNotFoundError(postgres_bin / name)
    editor = options.engine.resolve() / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    project = root / "AkUGCPlatform.uproject"
    if not editor.is_file():
        raise FileNotFoundError(editor)

    metadata_path = output / "published.json"
    run_checked(
        [
            editor,
            project,
            "-run=AkUGCPackTool",
            "-PublishWork",
            f"-Library={output / 'creator-a'}",
            f"-Output={metadata_path}",
            "-unattended",
            "-nop4",
            "-nosplash",
            "-nullrhi",
        ],
        output / "publish.log",
        120,
        root,
    )
    published = json.loads(metadata_path.read_text(encoding="utf-8-sig"))
    share = Path(published["sharePath"])
    artifact = share.read_bytes()
    public_key = published["publicKey"]
    release_digits = published["releaseId"]
    release_id = str(uuid.UUID(release_digits)).lower()

    pack = json.loads(artifact.decode("utf-8-sig"))
    pack["document"]["manifest"]["displayName"] = "Tampered after signing"
    tampered_path = output / "tampered.akugc"
    tampered_path.write_text(
        json.dumps(pack, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )
    tampered = tampered_path.read_bytes()

    tokens = {
        "creator": secrets.token_urlsafe(32),
        "reviewer": secrets.token_urlsafe(32),
        "player": secrets.token_urlsafe(32),
    }
    auth = {
        "principals": [
            {
                "subject": "creator-a",
                "tokenSha256": hashlib.sha256(tokens["creator"].encode()).hexdigest(),
                "roles": ["creator", "publisher"],
                "publisherKeys": [public_key],
            },
            {
                "subject": "reviewer",
                "tokenSha256": hashlib.sha256(tokens["reviewer"].encode()).hexdigest(),
                "roles": ["reviewer"],
                "publisherKeys": [],
            },
            {
                "subject": "player-b",
                "tokenSha256": hashlib.sha256(tokens["player"].encode()).hexdigest(),
                "roles": ["player"],
                "publisherKeys": [],
            },
        ]
    }
    auth_path = output / "auth.json"
    auth_path.write_text(json.dumps(auth, indent=2), encoding="utf-8")

    registry = output / "registry.exe"
    run_checked(
        ["go", "build", "-trimpath", "-o", registry, "./cmd/registry"],
        output / "go-build.log",
        120,
        root / "Services/ReleaseRegistry",
    )

    pgdata = output / "postgres"
    run_checked(
        [
            postgres_bin / "initdb.exe",
            "-D",
            pgdata,
            "-U",
            "postgres",
            "-A",
            "trust",
            "--encoding=UTF8",
            "--locale=C",
        ],
        output / "initdb.log",
        120,
    )
    postgres_port = free_port()
    postgres_log = output / "postgres.log"
    run_checked(
        [
            postgres_bin / "pg_ctl.exe",
            "-D",
            pgdata,
            "-l",
            postgres_log,
            "-o",
            f"-p {postgres_port} -h 127.0.0.1",
            "start",
        ],
        output / "pg-start.log",
        45,
    )
    postgres_started = True

    server = None
    server_stream = None
    try:
        port = free_port()
        base = f"http://127.0.0.1:{port}"
        validator_temp = output / "validator-temp"
        validator_temp.mkdir()
        environment = dict(os.environ)
        environment.update(
            {
                "DATABASE_URL": f"postgres://postgres@127.0.0.1:{postgres_port}/postgres?sslmode=disable",
                "AKUGC_REGISTRY_AUTH_CONFIG": str(auth_path),
                "AKUGC_VALIDATOR_EXE": str(editor),
                "AKUGC_VALIDATOR_PROJECT": str(project),
                "AKUGC_VALIDATOR_TEMP": str(validator_temp),
                "AKUGC_REGISTRY_ADDR": f"127.0.0.1:{port}",
            }
        )
        server_stream = (output / "registry.log").open("w", encoding="utf-8")
        server = subprocess.Popen(
            [str(registry)],
            cwd=root,
            env=environment,
            stdout=server_stream,
            stderr=subprocess.STDOUT,
        )
        wait_for_health(base, server)

        submit_headers = {
            "Content-Type": "application/vnd.akugc+json",
            "Idempotency-Key": "registry-concurrent-submit-v1",
        }
        with ThreadPoolExecutor(max_workers=8) as executor:
            submissions = list(
                executor.map(
                    lambda _: request(
                        base,
                        "POST",
                        "/v1/releases",
                        tokens["creator"],
                        artifact,
                        submit_headers,
                    ),
                    range(8),
                )
            )
        statuses = [value[0] for value in submissions]
        if statuses.count(201) != 1 or statuses.count(200) != 7:
            raise RuntimeError(f"Concurrent retry verdict mismatch: {statuses}")

        release_path = f"/v1/releases/{release_id}"
        status, _, _ = request(
            base,
            "GET",
            release_path + "/download",
            tokens["player"],
        )
        if status != 404:
            raise RuntimeError(f"Unpublished download returned {status}")

        status, _, _ = request(
            base,
            "POST",
            "/v1/releases",
            tokens["player"],
            artifact,
            {
                "Content-Type": "application/vnd.akugc+json",
                "Idempotency-Key": "registry-unauthorized-v1",
            },
        )
        if status != 403:
            raise RuntimeError(f"Unauthorized upload returned {status}")

        status, _, _ = request(
            base,
            "POST",
            "/v1/releases",
            tokens["creator"],
            artifact + b"\n",
            {
                "Content-Type": "application/vnd.akugc+json",
                "Idempotency-Key": "registry-id-collision-v1",
            },
        )
        if status != 409:
            raise RuntimeError(f"Release ID collision returned {status}")

        status, _, _ = request(
            base,
            "POST",
            "/v1/releases",
            tokens["creator"],
            tampered,
            {
                "Content-Type": "application/vnd.akugc+json",
                "Idempotency-Key": "registry-tampered-v1",
            },
        )
        if status != 400:
            raise RuntimeError(f"Tampered upload returned {status}")

        status, _, _ = request(
            base,
            "POST",
            release_path + "/review",
            tokens["reviewer"],
            json_body({"decision": "approved", "reason": "local review"}),
            {"Content-Type": "application/json"},
        )
        if status != 200:
            raise RuntimeError(f"Review returned {status}")
        status, _, _ = request(
            base,
            "POST",
            release_path + "/publish",
            tokens["creator"],
            b"",
        )
        if status != 200:
            raise RuntimeError(f"Publish returned {status}")

        status, headers, downloaded = request(
            base,
            "GET",
            release_path + "/download",
            tokens["player"],
        )
        if status != 200 or downloaded != artifact:
            raise RuntimeError(f"Published download mismatch: {status} {len(downloaded)}")
        expected_sha = hashlib.sha256(artifact).hexdigest()
        if headers.get("Etag") != f'"sha256:{expected_sha}"':
            raise RuntimeError("Artifact ETag mismatch")
        download_path = output / "player-b" / f"{release_digits}.akugc"
        download_path.parent.mkdir()
        download_path.write_bytes(downloaded)

        local_validation = output / "player-local-validation.json"
        run_checked(
            [
                editor,
                project,
                "-run=AkUGCPackTool",
                "-ValidateWork",
                f"-Input={download_path}",
                f"-Output={local_validation}",
                f"-TrustedKey={public_key}",
                "-unattended",
                "-nop4",
                "-nosplash",
                "-nullrhi",
            ],
            output / "player-local-validation.log",
            120,
            root,
        )

        game_completed = False
        client = (
            options.client.resolve()
            if options.client
            else root
            / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
        )
        if not options.skip_play:
            if not client.is_file():
                raise FileNotFoundError(client)
            play_log = output / "player-play.log"
            play_stdout = output / "player-play.stdout.log"
            run_checked(
                [
                    client,
                    "/Game/Maps/Phase0",
                    "-AkUGCWorkPlay",
                    f"-WorkId={release_digits}",
                    f"-WorkLibrary={output / 'player-b-library'}",
                    f"-WorkShare={download_path}",
                    f"-WorkTrustedKey={public_key}",
                    f"-WorkBadShare={tampered_path}",
                    "-ExecCmds=Automation RunTests AkUGC.Runtime.WorkLibrary.SharedPlayable",
                    "-TestExit=Automation Test Queue Empty",
                    "-unattended",
                    "-nop4",
                    "-nosplash",
                    "-nullrhi",
                    "-stdout",
                    f"-abslog={play_log}",
                ],
                play_stdout,
                190,
                client.parent if options.client else root,
            )
            require_automation_success(
                play_stdout.read_text(encoding="utf-8", errors="replace"),
                expected_selector="AkUGC.Runtime.WorkLibrary.SharedPlayable",
                expected_paths=("AkUGC.Runtime.WorkLibrary.SharedPlayable",),
                expected_total=1,
                require_queue_empty=True,
                require_test_exit=True,
                return_code=0,
            )
            if "[AkUGCWorkPlay] Passed" not in play_log.read_text(
                encoding="utf-8", errors="replace"
            ):
                raise RuntimeError("Downloaded work did not complete a game")
            game_completed = True

        status, _, _ = request(
            base,
            "POST",
            release_path + "/revoke",
            tokens["creator"],
            json_body({"reason": "local withdrawal"}),
            {"Content-Type": "application/json"},
        )
        if status != 200:
            raise RuntimeError(f"Revoke returned {status}")
        status, _, _ = request(
            base,
            "GET",
            release_path + "/download",
            tokens["player"],
        )
        if status != 404:
            raise RuntimeError(f"Revoked download returned {status}")

        result = {
            "passed": True,
            "release_id": release_id,
            "artifact_sha256": expected_sha,
            "concurrent_submit_statuses": sorted(statuses),
            "unauthorized_upload_rejected": True,
            "tampered_upload_rejected": True,
            "release_id_collision_rejected": True,
            "unpublished_download_rejected": True,
            "revoked_download_rejected": True,
            "player_local_validation": True,
            "player_completed_game": game_completed,
            "isolated_creator_reviewer_player": True,
            "postgresql": True,
            "same_machine_not_physical_devices": True,
        }
        result_path = output / "result.json"
        result_path.write_text(
            json.dumps(result, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        print(result_path)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    finally:
        if server is not None and server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=15)
        if server_stream is not None:
            server_stream.close()
        if postgres_started:
            run_checked(
                [
                    postgres_bin / "pg_ctl.exe",
                    "-D",
                    pgdata,
                    "-m",
                    "fast",
                    "stop",
                ],
                output / "pg-stop.log",
                45,
            )


if __name__ == "__main__":
    main()
