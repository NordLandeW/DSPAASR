"""Inspect built DLLs or a package ZIP without loading code or requiring a PE library."""
import argparse
import re
import struct
import sys
import zipfile
from pathlib import Path


OWN_NATIVE = {"dspaanative.dll", "dspaapresentbootstrap.dll"}
DYNAMIC_CRT = re.compile(
    r"(?:(?:msvcp|msvcr|vcruntime|concrt)\d[^/\\]*|ucrtbased?|api-ms-win-crt-[^/\\]+)\.dll",
    re.IGNORECASE,
)


def pe_imports(data):
    """Return normal/delay imports for a native x64 PE, or None for pure managed IL."""
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError("Truncated PE image")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError("Missing DOS signature")
    pe, = unpack("<I", 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Missing PE signature")
    machine, section_count = unpack("<HH", pe + 4)
    optional_size, = unpack("<H", pe + 20)
    optional = pe + 24
    magic, = unpack("<H", optional)
    if magic == 0x20B:
        directory_offset, count_offset = 112, 108
        image_base, = unpack("<Q", optional + 24)
    elif magic == 0x10B:
        directory_offset, count_offset = 96, 92
        image_base, = unpack("<I", optional + 28)
    else:
        raise ValueError("Unknown PE optional header")
    directory_count, = unpack("<I", optional + count_offset)
    if directory_offset + 8 * directory_count > optional_size:
        raise ValueError("PE data directories exceed the optional header")
    header_size, = unpack("<I", optional + 60)
    sections = []
    for index in range(section_count):
        entry = optional + optional_size + index * 40
        _, virtual_address, raw_size, raw_offset = unpack("<IIII", entry + 8)
        sections.append((virtual_address, raw_size, raw_offset))

    def offset_of(rva, size=1):
        if 0 <= rva < header_size and rva + size <= min(header_size, len(data)):
            return rva
        for address, length, offset in sections:
            if address <= rva and rva + size <= address + length:
                result = offset + rva - address
                if result + size <= len(data):
                    return result
        raise ValueError(f"Unmapped PE RVA 0x{rva:x}")

    def directory(index):
        if index >= directory_count:
            return 0, 0
        return unpack("<II", optional + directory_offset + index * 8)

    cli_rva, _ = directory(14)
    if cli_rva:
        flags, = unpack("<I", offset_of(cli_rva, 20) + 16)
        if flags & 1:  # COMIMAGE_FLAGS_ILONLY; mixed-mode images still need inspection.
            return None
    if machine != 0x8664 or magic != 0x20B:
        raise ValueError("Native payload must be Windows x64")

    imports = {"normal": [], "delay": []}
    for index, kind, stride in ((1, "normal", 20), (13, "delay", 32)):
        rva, length = directory(index)
        if not rva and not length:
            continue
        if not rva or length < stride:
            raise ValueError(f"Invalid {kind} import directory")
        terminated = False
        for position in range(0, length - stride + 1, stride):
            values = unpack("<" + "I" * (stride // 4), offset_of(rva + position, stride))
            if not any(values):
                terminated = True
                break
            name_rva = values[3] if kind == "normal" else values[1]
            if kind == "delay" and not (values[0] & 1):
                name_rva -= image_base
            start = offset_of(name_rva)
            end = data.find(b"\0", start, min(start + 4096, len(data)))
            if end < 0:
                raise ValueError(f"Unterminated {kind} import name")
            name = data[start:end].decode("ascii")
            if not name:
                raise ValueError(f"Empty {kind} import name")
            imports[kind].append(name)
        if not terminated:
            raise ValueError(f"Unterminated {kind} import directory")
    return imports


def inspect(paths, report_only=False):
    failures = []
    count = 0

    def inspect_image(name, data, enforce_static):
        nonlocal count
        try:
            imports = pe_imports(data)
            if imports is None:
                if enforce_static:
                    raise ValueError("Expected a native image, found managed IL")
                print(f"{name}: managed IL (native import check skipped)")
                return
            count += 1
            dynamic = []
            for kind, dependencies in imports.items():
                print(f"{name} [{kind}]: {', '.join(dependencies) or '(none)'}")
                for dependency in dependencies:
                    if DYNAMIC_CRT.fullmatch(dependency):
                        dynamic.append(dependency)
                        if enforce_static:
                            failures.append(f"{name} [{kind}] imports dynamic CRT {dependency}")
            if dynamic and not enforce_static:
                print(f"Vendor runtime requirement: {name}: {', '.join(sorted(set(dynamic)))}")
        except (ValueError, UnicodeError, struct.error) as error:
            # Malformed inputs must fail even in report-only mode.
            raise ValueError(f"{name}: {error}") from error

    for path in paths:
        if path.suffix.lower() == ".zip":
            with zipfile.ZipFile(path) as archive:
                if "DSPAANative.dll" not in archive.namelist():
                    raise ValueError(f"{path.name}: package is missing DSPAANative.dll")
                for entry in archive.infolist():
                    if entry.filename.lower().endswith(".dll"):
                        enforce_static = Path(entry.filename).name.lower() in OWN_NATIVE
                        inspect_image(entry.filename, archive.read(entry), enforce_static)
        else:
            inspect_image(path.name, path.read_bytes(), True)
    if not count:
        raise ValueError("No native DLLs were inspected")
    if failures:
        for failure in failures:
            print(f"Dynamic runtime dependency: {failure}", file=sys.stderr)
        if not report_only:
            raise ValueError("Project-owned native image requires a dynamic C/C++ runtime")
    print(f"Inspected {count} native x64 images (normal and delay imports).")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path,
                        help="Built native DLLs (strict), or package ZIPs (strict for project DLLs, report vendors)")
    parser.add_argument("--report-only", action="store_true", help="Report dynamic CRT imports without rejecting them")
    args = parser.parse_args()
    try:
        inspect(args.paths, args.report_only)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Native dependency check failed: {error}\n")


if __name__ == "__main__":
    main()
