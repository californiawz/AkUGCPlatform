import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import threading
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


def player_environment(token, bad_token=None):
    environment = dict(os.environ)
    environment["AKUGC_PLAYER_REGISTRY_TOKEN"] = token
    if bad_token is not None:
        environment["AKUGC_PLAYER_REGISTRY_BAD_TOKEN"] = bad_token
    return environment


def run_client_automation(
    client,
    root,
    output,
    name,
    test_path,
    arguments,
    timeout,
    business_marker,
    environment=None,
):
    play_log = output / f"{name}.log"
    play_stdout = output / f"{name}.stdout.log"
    run_checked(
        [
            client,
            "/Game/Maps/Phase0",
            *arguments,
            f"-ExecCmds=Automation RunTests {test_path}",
            "-TestExit=Automation Test Queue Empty",
            "-unattended",
            "-nop4",
            "-nosplash",
            "-nullrhi",
            "-stdout",
            f"-abslog={play_log}",
        ],
        play_stdout,
        timeout,
        client.parent,
        environment,
    )
    require_automation_success(
        play_stdout.read_text(encoding="utf-8", errors="replace"),
        expected_selector=test_path,
        expected_paths=(test_path,),
        expected_total=1,
        require_queue_empty=True,
        require_test_exit=True,
        return_code=0,
    )
    business_text = play_stdout.read_text(
        encoding="utf-8", errors="replace"
    )
    if business_marker not in business_text:
        business_text += play_log.read_text(
            encoding="utf-8", errors="replace"
        )
    if business_marker not in business_text:
        raise RuntimeError(f"{name} did not emit {business_marker}")


@contextmanager
def artifact_fixture(mode, release_id, artifact, content_hash):
    expected_path = f"/v1/releases/{release_id}/download"
    state = {"paths": [], "authorization": []}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, _format, *_args):
            return

        def do_GET(self):
            state["paths"].append(self.path)
            state["authorization"].append(self.headers.get("Authorization"))
            if mode == "redirect" and self.path == expected_path:
                port = self.server.server_address[1]
                self.send_response(302)
                self.send_header(
                    "Location",
                    f"http://127.0.0.1:{port}/redirect-target",
                )
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return

            body = artifact
            if mode == "oversize":
                body = b""
            if self.path not in (expected_path, "/redirect-target"):
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return

            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.akugc+json")
            if mode == "oversize":
                self.send_header("Content-Length", str(16 * 1024 * 1024 + 1))
                artifact_sha = "0" * 64
            else:
                self.send_header("Content-Length", str(len(body)))
                artifact_sha = hashlib.sha256(body).hexdigest()
            self.send_header("ETag", f'"sha256:{artifact_sha}"')
            self.send_header("X-AkUGC-Content-Hash", content_hash)
            self.send_header("Connection", "close")
            self.end_headers()
            if body:
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}", state
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


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
    client = (
        options.client.resolve()
        if options.client
        else root
        / "Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe"
    )
    if not options.skip_play and not client.is_file():
        raise FileNotFoundError(client)

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
    project_id = str(uuid.UUID(pack["manifest"]["projectId"])).lower()
    pack["document"]["manifest"]["displayName"] = "Tampered after signing"
    tampered_path = output / "tampered.akugc"
    tampered_path.write_text(
        json.dumps(pack, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )
    tampered = tampered_path.read_bytes()

    tokens = {
        "creator": secrets.token_urlsafe(32),
        "editor": secrets.token_urlsafe(32),
        "viewer": secrets.token_urlsafe(32),
        "outsider": secrets.token_urlsafe(32),
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
                "subject": "creator-editor",
                "tokenSha256": hashlib.sha256(tokens["editor"].encode()).hexdigest(),
                "roles": ["creator", "publisher"],
                "publisherKeys": [public_key],
            },
            {
                "subject": "project-viewer",
                "tokenSha256": hashlib.sha256(tokens["viewer"].encode()).hexdigest(),
                "roles": ["player"],
                "publisherKeys": [],
            },
            {
                "subject": "project-outsider",
                "tokenSha256": hashlib.sha256(tokens["outsider"].encode()).hexdigest(),
                "roles": ["player"],
                "publisherKeys": [],
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
    game_completed = False
    unpublished_product_rejected = False
    wrong_token_rejected = False
    wrong_key_rejected = False
    tampered_download_rejected = False
    oversized_download_rejected = False
    redirect_rejected = False
    revoked_product_rejected = False
    try:
        test_environment = dict(os.environ)
        test_environment["AKUGC_TEST_DATABASE_URL"] = (
            f"postgres://postgres@127.0.0.1:{postgres_port}/postgres?sslmode=disable"
        )
        run_checked(
            ["go", "test", "-count=1", "./..."],
            output / "go-postgres-tests.log",
            180,
            root / "Services/ReleaseRegistry",
            test_environment,
        )
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

        project_path = f"/v1/projects/{project_id}"
        project_headers = {
            "Content-Type": "application/json",
            "Idempotency-Key": "registry-create-project-v1",
        }
        project_body = json_body(
            {
                "projectId": project_id,
                "displayName": "Release Registry verification",
            }
        )
        status, _, raw = request(
            base,
            "POST",
            "/v1/projects",
            tokens["creator"],
            project_body,
            project_headers,
        )
        if status != 201:
            raise RuntimeError(f"Project creation returned {status}: {raw!r}")
        created_project = json.loads(raw)
        if (
            created_project["project"]["ownerSubject"] != "creator-a"
            or created_project["project"]["visibility"] != "private"
        ):
            raise RuntimeError("Project owner binding mismatch")
        status, _, raw = request(
            base,
            "POST",
            "/v1/projects",
            tokens["creator"],
            project_body,
            project_headers,
        )
        if status != 200 or json.loads(raw)["created"]:
            raise RuntimeError(f"Project idempotent retry returned {status}: {raw!r}")
        for subject, role in (
            ("creator-editor", "editor"),
            ("project-viewer", "viewer"),
        ):
            status, _, raw = request(
                base,
                "PUT",
                f"{project_path}/members/{subject}",
                tokens["creator"],
                json_body({"role": role}),
                {"Content-Type": "application/json"},
            )
            if status != 200 or not json.loads(raw)["changed"]:
                raise RuntimeError(
                    f"Project member {subject} creation returned {status}: {raw!r}"
                )
        status, _, _ = request(
            base,
            "GET",
            project_path,
            tokens["outsider"],
        )
        if status != 404:
            raise RuntimeError(f"Outsider project lookup returned {status}")
        status, _, raw = request(
            base,
            "GET",
            project_path + "/members",
            tokens["viewer"],
        )
        members = json.loads(raw)["members"] if status == 200 else []
        if [(member["subject"], member["role"]) for member in members] != [
            ("creator-a", "owner"),
            ("creator-editor", "editor"),
            ("project-viewer", "viewer"),
        ]:
            raise RuntimeError(
                f"Project member listing mismatch: status={status} members={members!r}"
            )

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
                        tokens["editor"],
                        artifact,
                        submit_headers,
                    ),
                    range(8),
                )
            )
        statuses = [value[0] for value in submissions]
        if statuses.count(201) != 1 or statuses.count(200) != 7:
            raise RuntimeError(f"Concurrent retry verdict mismatch: {statuses}")
        release_responses = [
            json.loads(value[2])["release"] for value in submissions
        ]
        if any(
            release["ownerSubject"] != "creator-a"
            or release["submittedBySubject"] != "creator-editor"
            for release in release_responses
        ):
            raise RuntimeError("Collaborator release attribution mismatch")
        status, _, raw = request(
            base,
            "GET",
            project_path + "/versions?limit=1",
            tokens["viewer"],
        )
        versions = json.loads(raw)["versions"] if status == 200 else []
        if (
            len(versions) != 1
            or versions[0]["releaseId"] != release_id
            or "artifact" in versions[0]
        ):
            raise RuntimeError(
                f"Project version listing mismatch: status={status} versions={versions!r}"
            )
        status, _, raw = request(
            base,
            "DELETE",
            project_path + "/members/creator-editor",
            tokens["creator"],
            b"",
        )
        if status != 200 or not json.loads(raw)["changed"]:
            raise RuntimeError(f"Editor removal returned {status}: {raw!r}")
        status, _, _ = request(
            base,
            "POST",
            "/v1/releases",
            tokens["editor"],
            artifact,
            {
                "Content-Type": "application/vnd.akugc+json",
                "Idempotency-Key": "registry-removed-editor-v1",
            },
        )
        if status != 403:
            raise RuntimeError(f"Removed editor upload returned {status}")

        release_path = f"/v1/releases/{release_id}"
        status, _, _ = request(
            base,
            "GET",
            release_path + "/download",
            tokens["player"],
        )
        if status != 404:
            raise RuntimeError(f"Unpublished download returned {status}")
        if not options.skip_play:
            run_client_automation(
                client,
                root,
                output,
                "player-unpublished-rejected",
                "AkUGC.Runtime.ReleaseRegistry.DownloadRejected",
                [
                    "-AkUGCRegistryReject",
                    f"-WorkId={release_digits}",
                    f"-WorkLibrary={output / 'player-unpublished-library'}",
                    f"-RegistryBase={base}",
                    f"-WorkTrustedKey={public_key}",
                    "-RegistryExpectedFailure=not_available",
                ],
                100,
                "[AkUGCPlayerRegistryReject] Passed",
                player_environment(tokens["player"]),
            )
            unpublished_product_rejected = True

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

        work_path = f"/v1/works/{project_id}"
        status, _, _ = request(
            base,
            "GET",
            work_path,
            tokens["player"],
        )
        if status != 404:
            raise RuntimeError(f"Private outsider work lookup returned {status}")
        status, _, _ = request(
            base,
            "GET",
            release_path + "/download",
            tokens["player"],
        )
        if status != 404:
            raise RuntimeError(f"Private outsider download returned {status}")

        status, _, raw = request(
            base,
            "GET",
            work_path,
            tokens["viewer"],
        )
        private_work = json.loads(raw)["work"] if status == 200 else {}
        if (
            private_work.get("visibility") != "private"
            or private_work.get("latestRelease", {}).get("releaseId") != release_id
        ):
            raise RuntimeError(
                f"Private member work lookup mismatch: {status} {private_work!r}"
            )
        status, _, member_download = request(
            base,
            "GET",
            release_path + "/download",
            tokens["viewer"],
        )
        if status != 200 or member_download != artifact:
            raise RuntimeError(f"Private member download mismatch: {status}")

        status, _, _ = request(
            base,
            "PUT",
            project_path + "/visibility",
            tokens["player"],
            json_body({"visibility": "public"}),
            {"Content-Type": "application/json"},
        )
        if status != 404:
            raise RuntimeError(f"Outsider visibility mutation returned {status}")
        status, _, _ = request(
            base,
            "PUT",
            project_path + "/visibility",
            tokens["creator"],
            b'{"visibility":"public","visibility":"private"}',
            {"Content-Type": "application/json"},
        )
        if status != 400:
            raise RuntimeError(f"Duplicate visibility request returned {status}")
        status, _, raw = request(
            base,
            "PUT",
            project_path + "/visibility",
            tokens["creator"],
            json_body({"visibility": "public"}),
            {"Content-Type": "application/json"},
        )
        visibility_response = json.loads(raw) if status == 200 else {}
        if (
            not visibility_response.get("changed")
            or visibility_response.get("project", {}).get("visibility") != "public"
        ):
            raise RuntimeError(
                f"Public visibility transition mismatch: {status} {raw!r}"
            )
        status, _, raw = request(
            base,
            "PUT",
            project_path + "/visibility",
            tokens["creator"],
            json_body({"visibility": "public"}),
            {"Content-Type": "application/json"},
        )
        if status != 200 or json.loads(raw).get("changed"):
            raise RuntimeError(f"Public visibility retry was not idempotent: {raw!r}")

        status, _, raw = request(
            base,
            "GET",
            work_path,
            tokens["player"],
        )
        public_work = json.loads(raw)["work"] if status == 200 else {}
        if (
            public_work.get("visibility") != "public"
            or public_work.get("latestRelease", {}).get("releaseId") != release_id
            or "artifact" in public_work.get("latestRelease", {})
        ):
            raise RuntimeError(
                f"Public work lookup mismatch: {status} {public_work!r}"
            )

        status, _, raw = request(
            base,
            "PUT",
            project_path + "/visibility",
            tokens["creator"],
            json_body({"visibility": "private"}),
            {"Content-Type": "application/json"},
        )
        if status != 200 or not json.loads(raw).get("changed"):
            raise RuntimeError(f"Private visibility transition mismatch: {raw!r}")
        for hidden_path in (work_path, release_path + "/download"):
            status, _, _ = request(
                base,
                "GET",
                hidden_path,
                tokens["player"],
            )
            if status != 404:
                raise RuntimeError(
                    f"Private visibility did not hide {hidden_path}: {status}"
                )
        status, _, raw = request(
            base,
            "PUT",
            project_path + "/visibility",
            tokens["creator"],
            json_body({"visibility": "public"}),
            {"Content-Type": "application/json"},
        )
        if status != 200 or not json.loads(raw).get("changed"):
            raise RuntimeError(f"Public visibility restore mismatch: {raw!r}")

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

        if not options.skip_play:
            fixture_token = "fixture-player-token-1234567890"
            fixture_content_hash = pack["manifest"]["contentHash"]
            with artifact_fixture(
                "tampered",
                release_id,
                tampered,
                fixture_content_hash,
            ) as (fixture_base, fixture_state):
                run_client_automation(
                    client,
                    root,
                    output,
                    "player-tampered-download-rejected",
                    "AkUGC.Runtime.ReleaseRegistry.DownloadRejected",
                    [
                        "-AkUGCRegistryReject",
                        f"-WorkId={release_digits}",
                        f"-WorkLibrary={output / 'player-tampered-library'}",
                        f"-RegistryBase={fixture_base}",
                        f"-WorkTrustedKey={public_key}",
                        "-RegistryExpectedFailure=local_verification",
                    ],
                    100,
                    "[AkUGCPlayerRegistryReject] Passed",
                    player_environment(fixture_token),
                )
            if fixture_state["paths"] != [release_path + "/download"]:
                raise RuntimeError(
                    f"Tampered fixture request mismatch: {fixture_state['paths']}"
                )
            tampered_download_rejected = True

            with artifact_fixture(
                "oversize",
                release_id,
                artifact,
                fixture_content_hash,
            ) as (fixture_base, fixture_state):
                run_client_automation(
                    client,
                    root,
                    output,
                    "player-oversized-download-rejected",
                    "AkUGC.Runtime.ReleaseRegistry.DownloadRejected",
                    [
                        "-AkUGCRegistryReject",
                        f"-WorkId={release_digits}",
                        f"-WorkLibrary={output / 'player-oversized-library'}",
                        f"-RegistryBase={fixture_base}",
                        f"-WorkTrustedKey={public_key}",
                        "-RegistryExpectedFailure=response_too_large",
                    ],
                    100,
                    "[AkUGCPlayerRegistryReject] Passed",
                    player_environment(fixture_token),
                )
            oversized_download_rejected = True

            with artifact_fixture(
                "redirect",
                release_id,
                artifact,
                fixture_content_hash,
            ) as (fixture_base, fixture_state):
                run_client_automation(
                    client,
                    root,
                    output,
                    "player-redirect-rejected",
                    "AkUGC.Runtime.ReleaseRegistry.DownloadRejected",
                    [
                        "-AkUGCRegistryReject",
                        f"-WorkId={release_digits}",
                        f"-WorkLibrary={output / 'player-redirect-library'}",
                        f"-RegistryBase={fixture_base}",
                        f"-WorkTrustedKey={public_key}",
                        "-RegistryExpectedFailure=redirect",
                    ],
                    100,
                    "[AkUGCPlayerRegistryReject] Passed",
                    player_environment(fixture_token),
                )
            if fixture_state["paths"] != [release_path + "/download"]:
                raise RuntimeError(
                    "Player followed a forbidden Registry redirect: "
                    + repr(fixture_state["paths"])
                )
            redirect_rejected = True

            bad_public_key = (
                ("0" if public_key[0] != "0" else "1") + public_key[1:]
            )
            run_client_automation(
                client,
                root,
                output,
                "player-registry-play",
                "AkUGC.Runtime.WorkLibrary.SharedPlayable",
                [
                    "-AkUGCWorkPlay",
                    "-RegistryExerciseRejections",
                    f"-WorkId={release_digits}",
                    f"-WorkLibrary={output / 'player-b-library'}",
                    f"-RegistryBase={base}",
                    f"-RegistryProjectId={project_id}",
                    f"-RegistryBadTrustedKey={bad_public_key}",
                    f"-WorkTrustedKey={public_key}",
                ],
                190,
                "[AkUGCWorkPlay] Passed",
                player_environment(
                    tokens["player"],
                    tokens["player"] + "-invalid",
                ),
            )
            play_text = (output / "player-registry-play.log").read_text(
                encoding="utf-8", errors="replace"
            )
            if "Source=Registry" not in play_text or "Rejections=2" not in play_text:
                raise RuntimeError(
                    "Player did not prove direct Registry acquisition and rejection gates"
                )
            wrong_token_rejected = True
            wrong_key_rejected = True
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
        status, _, _ = request(
            base,
            "GET",
            work_path,
            tokens["player"],
        )
        if status != 404:
            raise RuntimeError(f"Revoked work page returned {status}")
        if not options.skip_play:
            run_client_automation(
                client,
                root,
                output,
                "player-revoked-rejected",
                "AkUGC.Runtime.ReleaseRegistry.DownloadRejected",
                [
                    "-AkUGCRegistryReject",
                    f"-WorkId={release_digits}",
                    f"-WorkLibrary={output / 'player-revoked-library'}",
                    f"-RegistryBase={base}",
                    f"-WorkTrustedKey={public_key}",
                    "-RegistryExpectedFailure=not_available",
                ],
                100,
                "[AkUGCPlayerRegistryReject] Passed",
                player_environment(tokens["player"]),
            )
            revoked_product_rejected = True

        result = {
            "passed": True,
            "release_id": release_id,
            "project_id": project_id,
            "artifact_sha256": expected_sha,
            "concurrent_submit_statuses": sorted(statuses),
            "project_creation_idempotent": True,
            "project_owner_bound": True,
            "collaborator_submitter_attributed": True,
            "project_member_listing_verified": True,
            "project_version_listing_bounded": True,
            "outsider_project_hidden": True,
            "project_default_private": True,
            "private_member_work_access": True,
            "private_outsider_hidden": True,
            "visibility_owner_only": True,
            "visibility_strict_json": True,
            "visibility_idempotent": True,
            "public_work_page_verified": True,
            "removed_editor_upload_rejected": True,
            "unauthorized_upload_rejected": True,
            "tampered_upload_rejected": True,
            "release_id_collision_rejected": True,
            "unpublished_download_rejected": True,
            "revoked_download_rejected": True,
            "player_local_validation": True,
            "player_project_work_page_download": game_completed,
            "player_unpublished_product_rejected": unpublished_product_rejected,
            "player_wrong_token_rejected": wrong_token_rejected,
            "player_wrong_key_rejected": wrong_key_rejected,
            "player_tampered_download_rejected": tampered_download_rejected,
            "player_oversized_download_rejected": oversized_download_rejected,
            "player_redirect_rejected": redirect_rejected,
            "player_revoked_product_rejected": revoked_product_rejected,
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
