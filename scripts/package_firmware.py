#!/usr/bin/env python3
"""Copy a built firmware into dist/ with a clear, versioned name + SHA256.

So you never have to guess which `firmware.bin` to flash again.

Usage:
    python scripts/package_firmware.py [env]      # env defaults to gh_release

Output name:
    dist/PocketBook-v<VERSION>-<branch>-<shortsha>[-dev].bin   (+ .sha256 sidecar)

  - VERSION comes from the repo-root VERSION file (single source of truth).
  - `-dev` suffix is added for any env other than gh_release (e.g. the `default`
    debug build, which has serial logging on).
  - gh_release is the clean build to flash for normal use.
"""
from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read_version() -> str:
    vf = ROOT / "VERSION"
    return vf.read_text(encoding="utf-8").strip() if vf.is_file() else "0.0.0"


def git(*args: str, default: str = "") -> str:
    try:
        out = subprocess.check_output(["git", *args], cwd=ROOT, text=True, stderr=subprocess.DEVNULL)
        return out.strip()
    except Exception:
        return default


def main() -> None:
    env = sys.argv[1] if len(sys.argv) > 1 else "gh_release"
    src = ROOT / ".pio" / "build" / env / "firmware.bin"
    if not src.is_file():
        print(f"ERROR: {src} not found. Build it first:  pio run -e {env}", file=sys.stderr)
        sys.exit(1)

    version = read_version()
    branch = git("rev-parse", "--abbrev-ref", "HEAD", default="nogit").replace("/", "-")
    sha = git("rev-parse", "--short", "HEAD", default="nosha")
    dev = "" if env == "gh_release" else "-dev"
    name = f"PocketBook-v{version}-{branch}-{sha}{dev}.bin"

    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    dst = dist / name
    shutil.copy2(src, dst)

    digest = hashlib.sha256(dst.read_bytes()).hexdigest()
    (dist / f"{name}.sha256").write_text(f"{digest}  {name}\n", encoding="utf-8")

    size = dst.stat().st_size
    print(f"Packaged: dist/{name}  ({size:,} bytes, {size / 1048576:.2f} MB)")
    print(f"SHA256:   {digest}")


if __name__ == "__main__":
    main()
