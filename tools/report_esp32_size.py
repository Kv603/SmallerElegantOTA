#!/usr/bin/env python3
"""Print the ESP32 ELF sections and OTA image size for a PlatformIO build."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


SECTIONS = (".text", ".rodata", ".data", ".bss")


def find_size_tool(explicit: str | None) -> str:
    if explicit:
        return explicit

    tool_name = "xtensa-esp32-elf-size"
    if shutil.which(tool_name):
        return tool_name

    pio_home = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    candidates = sorted(pio_home.glob(f"packages/toolchain-xtensa-esp-elf/bin/{tool_name}"))
    if candidates:
        return str(candidates[-1])
    raise FileNotFoundError(
        f"Could not find {tool_name}. Pass --size-tool or install the ESP32 PlatformIO toolchain."
    )


def sections_from_elf(size_tool: str, elf: Path) -> dict[str, int]:
    result = subprocess.run(
        [size_tool, "-A", str(elf)], check=True, text=True, capture_output=True
    )
    sections = {section: 0 for section in SECTIONS}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] in sections:
            sections[fields[0]] = int(fields[1], 0)
    return sections


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True, help="Path to the compiled ELF file")
    parser.add_argument("--map", dest="map_file", type=Path, required=True, help="Path to the linker map")
    parser.add_argument("--bin", dest="image", type=Path, help="Path to the OTA .bin image")
    parser.add_argument("--size-tool", help="Path to xtensa-esp32-elf-size")
    parser.add_argument("--json", action="store_true", help="Emit a JSON report")
    args = parser.parse_args()

    for artifact in (args.elf, args.map_file):
        if not artifact.is_file():
            parser.error(f"artifact does not exist: {artifact}")
    image = args.image or args.elf.with_suffix(".bin")
    if not image.is_file():
        parser.error(f"image does not exist: {image}")

    sections = sections_from_elf(find_size_tool(args.size_tool), args.elf)
    report = {
        "elf": str(args.elf),
        "map": str(args.map_file),
        "image": str(image),
        "image_size": image.stat().st_size,
        "sections": sections,
    }
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print("ESP32 async size report")
        print(f"ELF: {args.elf}")
        print(f"Map: {args.map_file}")
        for section in SECTIONS:
            print(f"{section:8} {sections[section]:>10} bytes")
        print(f"image     {report['image_size']:>10} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
