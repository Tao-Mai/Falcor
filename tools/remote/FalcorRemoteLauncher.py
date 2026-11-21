"""Download and run the Falcor viewer from a trusted SSH host."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import socket
import subprocess
import sys
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def receive_bundle(stream, cached):
    header = stream.readline(128).decode("ascii").split()
    if len(header) != 2:
        raise ValueError("SSH did not return a viewer version. Check the SSH host and --repo path.")
    version, size = header
    size = int(size)
    if not 0 <= size <= 4 * 1024 * 1024:
        raise ValueError("Unexpected viewer bundle size")
    data = stream.read(size) if size else cached
    if (size and len(data) != size) or digest(data) != version:
        raise ValueError("The viewer download did not match the server's version")
    bundle = json.loads(data)
    if not isinstance(bundle["source"], str) or not isinstance(bundle["requirements"], str):
        raise ValueError("Invalid viewer bundle")
    return data, bundle, version


def ensure_packages(cache, requirements, temp):
    requirements_path = temp / "requirements.txt"
    requirements_path.write_text(requirements, encoding="utf-8")
    version = digest(requirements.encode("utf-8"))
    packages = cache / f"packages-{sys.implementation.cache_tag}-{version}"
    if packages.is_dir():
        return packages

    print("Installing the viewer's Python packages…", flush=True)
    staging = temp / "packages"
    subprocess.run(
        [
            sys.executable, "-m", "pip", "install", "--disable-pip-version-check",
            "--only-binary=:all:", "--target", str(staging), "-r", str(requirements_path),
        ],
        check=True,
    )
    try:
        staging.rename(packages)
    except OSError:
        if not packages.is_dir():
            raise
    return packages


def launch(args):
    cache_base = Path(os.environ.get("LOCALAPPDATA", Path.home() / ".cache"))
    identity = f"{args.host}\0{args.repo}".encode("utf-8")
    cache = cache_base / "FalcorRemote" / digest(identity)[:16]
    cache.mkdir(parents=True, exist_ok=True)
    bundle_file = cache / "viewer.bundle"
    cached = bundle_file.read_bytes() if bundle_file.is_file() else b""

    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        local_port = reservation.getsockname()[1]
    script = PurePosixPath(args.repo) / "Source/Remote/serve_viewer.py"
    command = shlex.join(["python3", "-I", str(script), digest(cached)])
    ssh = [
        "ssh", "-T", "-o", "ExitOnForwardFailure=yes", "-o", "ConnectTimeout=10",
        "-L", f"127.0.0.1:{local_port}:127.0.0.1:{args.port}", "--", args.host, command,
    ]
    with subprocess.Popen(ssh, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as tunnel:
        try:
            data, bundle, version = receive_bundle(tunnel.stdout, cached)
            with tempfile.TemporaryDirectory(dir=cache) as directory:
                temp = Path(directory)
                if data != cached:
                    staged_bundle = temp / "viewer.bundle"
                    staged_bundle.write_bytes(data)
                    staged_bundle.replace(bundle_file)
                    print(f"Updated viewer to {version[:12]}.", flush=True)
                else:
                    print(f"Viewer is up to date ({version[:12]}).", flush=True)

                packages = ensure_packages(cache, bundle["requirements"], temp)
                viewer = temp / "viewer.py"
                viewer.write_text(bundle["source"], encoding="utf-8")
                env = dict(os.environ, PYTHONPATH=str(packages), PYTHONNOUSERSITE="1")
                return subprocess.call(
                    [sys.executable, str(viewer), "127.0.0.1", str(local_port)],
                    env=env,
                )
        finally:
            tunnel.stdin.close()
            try:
                tunnel.wait(timeout=2)
            except subprocess.TimeoutExpired:
                tunnel.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="SSH alias or user@host")
    parser.add_argument("--repo", default="project/rendering/Falcor", help="Falcor checkout on the SSH host")
    parser.add_argument("--port", type=int, default=9000, help="Mogwai remote port on the SSH host")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    try:
        return launch(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"FalcorRemoteLauncher: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
