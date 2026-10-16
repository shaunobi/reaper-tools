#!/usr/bin/env python3
"""Adds a version of the SRT extension package to the ReaPack index.

Usage:
  reapack_index.py --index index.xml --version 0.1.0 --author shaunobi \
      --base-url https://github.com/shaunobi/reaper-tools/releases/download/v0.1.0 \
      --changelog CHANGES.txt \
      --files win64=reaper_srt.dll,onnxruntime.dll,onnxruntime_providers_shared.dll,DirectML.dll \
      --files darwin-arm64=reaper_srt.dylib,libonnxruntime.1.24.4.dylib

Creates the index if it does not exist, keeps earlier versions, and replaces
an entry with the same version name. Every listed file is expected to be
attached to the release under its own name and installs into UserPlugins.
"""
import argparse
import datetime
import pathlib
import xml.etree.ElementTree as ET

PACKAGE = "Shaunobi's Reaper Tools.ext"
DESC = "SRT: stem separation, drum alignment to the grid"
CATEGORY = "Extensions"
INDEX_NAME = "Shaunobi's Reaper Tools"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--index", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--author", required=True)
    ap.add_argument("--base-url", required=True)
    ap.add_argument("--changelog", help="text file with the changelog for this version")
    ap.add_argument("--files", action="append", required=True, help="platform=file1,file2,...")
    ap.add_argument("--time", help="RFC3339 timestamp; default now (UTC)")
    args = ap.parse_args()

    path = pathlib.Path(args.index)
    if path.exists():
        root = ET.parse(path).getroot()
    else:
        root = ET.Element("index", version="1", name=INDEX_NAME)
    cat = next((c for c in root.findall("category") if c.get("name") == CATEGORY), None)
    if cat is None:
        cat = ET.SubElement(root, "category", name=CATEGORY)
    pkg = next((p for p in cat.findall("reapack") if p.get("name") == PACKAGE), None)
    if pkg is None:
        pkg = ET.SubElement(cat, "reapack", name=PACKAGE, type="extension", desc=DESC)
        meta = ET.SubElement(pkg, "metadata")
        ET.SubElement(meta, "link", rel="website").text = "https://github.com/shaunobi/reaper-tools"
    for old in [v for v in pkg.findall("version") if v.get("name") == args.version]:
        pkg.remove(old)

    stamp = args.time or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    ver = ET.SubElement(pkg, "version", name=args.version, author=args.author, time=stamp)
    if args.changelog:
        ET.SubElement(ver, "changelog").text = pathlib.Path(args.changelog).read_text().strip()
    for spec in args.files:
        platform, names = spec.split("=", 1)
        for name in [n for n in names.split(",") if n]:
            src = ET.SubElement(ver, "source", platform=platform, file=name)
            src.text = f"{args.base_url.rstrip('/')}/{name}"

    ET.indent(root, space="  ")
    path.write_text('<?xml version="1.0" encoding="utf-8"?>\n' + ET.tostring(root, encoding="unicode") + "\n")
    print(f"{path}: added {PACKAGE} {args.version} with {sum(len(v) for v in [s.split('=',1)[1].split(',') for s in args.files])} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
