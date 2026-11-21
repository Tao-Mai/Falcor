"""Send the current Python viewer to the launcher over its SSH session."""

import hashlib
import json
from pathlib import Path
import sys


def main(cached_version):
    source_dir = Path(__file__).resolve().parent
    payload = json.dumps(
        {
            "source": (source_dir / "viewer.py").read_text(encoding="utf-8"),
            "requirements": (source_dir / "requirements.txt").read_text(encoding="utf-8"),
        },
        separators=(",", ":"),
    ).encode("utf-8")
    version = hashlib.sha256(payload).hexdigest()
    output = sys.stdout.buffer
    if version == cached_version:
        output.write(f"{version} 0\n".encode("ascii"))
    else:
        output.write(f"{version} {len(payload)}\n".encode("ascii"))
        output.write(payload)
    output.flush()
    # Keep the SSH tunnel open until the local launcher closes the session.
    sys.stdin.buffer.read(1)


if __name__ == "__main__":
    main(sys.argv[1])
