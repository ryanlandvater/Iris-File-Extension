#!/usr/bin/env python3
"""Fetch the hosted corpus into a local cache, verifying size and SHA-256.

The manifest (tests/corpus/manifest.json) is the committed source of truth:
URL, byte size, digest, IFE version, and the block types each fixture covers.
The files themselves live on iris.exampleslides.org (Cloudflare R2) and are
never committed — see plans/phase-6-corpus.md.

Behaviour, by design:
  * A cached file whose digest matches the manifest is used as-is; the normal
    path never touches the network (CI caches .deps/corpus by manifest digest).
  * Anything missing or mismatched is (re)downloaded, and the download must
    match size AND digest before it replaces the cache. A re-uploaded file
    that the manifest has not blessed is rejected, not silently adopted.
  * Any failure exits non-zero. A gate that disables itself on a network
    error is the ctest-from-stale-binaries failure mode wearing a different
    hat (MIGRATION.md, Phase 6 traps).
"""
import argparse
import hashlib
import json
import pathlib
import sys
import urllib.error
import urllib.request


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url: str, dest: pathlib.Path, expected_size: int, expected_sha: str) -> None:
    tmp = dest.with_name(dest.name + ".tmp")
    try:
        with urllib.request.urlopen(url, timeout=30) as resp:
            with open(tmp, "wb") as out:
                n = 0
                while True:
                    chunk = resp.read(1 << 20)
                    if not chunk:
                        break
                    n += len(chunk)
                    out.write(chunk)
        if n != expected_size:
            raise SystemExit(
                f"fetch_corpus: {url}: got {n} bytes, manifest declares {expected_size}"
            )
        if sha256(tmp) != expected_sha:
            raise SystemExit(
                f"fetch_corpus: {url}: SHA-256 mismatch — the hosted file differs "
                f"from what tests/corpus/manifest.json pins. Re-upload the file "
                f"that produced the manifest digest, or update the manifest."
            )
        tmp.replace(dest)
    except urllib.error.URLError as e:
        raise SystemExit(f"fetch_corpus: {url}: unreachable ({e})")
    finally:
        tmp.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", required=True, type=pathlib.Path)
    ap.add_argument("--dest", required=True, type=pathlib.Path)
    args = ap.parse_args()

    manifest = json.loads(args.manifest.read_text())
    args.dest.mkdir(parents=True, exist_ok=True)

    for name, entry in manifest.items():
        target = args.dest / name
        if (
            target.is_file()
            and target.stat().st_size == entry["size"]
            and sha256(target) == entry["sha256"]
        ):
            print(f"fetch_corpus: {name}: cached, digest ok")
            continue
        print(f"fetch_corpus: {name}: fetching {entry['url']}")
        fetch(entry["url"], target, entry["size"], entry["sha256"])

    print("fetch_corpus: all corpus files present and digest-verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
