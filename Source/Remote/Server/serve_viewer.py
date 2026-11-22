"""Send the current Python viewer to the launcher over its SSH session."""

import hashlib
import json
from pathlib import Path
import sys


def main(cached_version):
    source_dir = Path(__file__).resolve().parent.parent / "Client"
    controls = (source_dir / "controls.py").read_text(encoding="utf-8")
    viewer = (source_dir / "viewer.py").read_text(encoding="utf-8")
    # The installed launcher expects a single entry script in the bundle.
    source = (
        '"""Launch the Falcor remote editor."""\n'
        "from pathlib import Path as _BundlePath\n"
        f"_BundlePath(__file__).with_name('controls.py').write_text({controls!r}, encoding='utf-8')\n"
        + viewer
    )
    payload = json.dumps(
        {
            "source": source,
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
