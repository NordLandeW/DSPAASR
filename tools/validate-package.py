"""Offline Thunderstore/Gale archive validation; does not install or upload."""
import hashlib
import json
import re
import struct
import sys
import zipfile


def validate(path):
    expected = {
        "manifest.json", "README.md", "LICENSE", "CHANGELOG.md", "icon.png", "DSPAAMod.dll",
        "DSPAANative.dll", "nvngx_dlss.dll", "NVIDIA-RTX-SDK-LICENSE.txt", "NVIDIA-DLSS-NOTICES.txt",
        "third-party.md", "SHA256SUMS.json",
        "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll", "AMD-FSR-SDK-LICENSE.md",
    }
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != expected:
            raise ValueError("Archive must contain exactly the intended flat-root package files")
        corrupt = archive.testzip()
        if corrupt:
            raise ValueError(f"ZIP CRC failure: {corrupt}")
        manifest = json.loads(archive.read("manifest.json"))
        if not re.fullmatch(r"[a-zA-Z0-9_]{1,128}", manifest["name"]):
            raise ValueError("Invalid package name")
        version = manifest["version_number"]
        if not re.fullmatch(r"\d+\.\d+\.\d+", version):
            raise ValueError("Invalid package version")
        if not isinstance(manifest["description"], str) or len(manifest["description"]) > 250:
            raise ValueError("Invalid description")
        website = manifest["website_url"]
        if not isinstance(website, str) or (website and not website.startswith("https://")):
            raise ValueError("Invalid website URL")
        if manifest["dependencies"] != ["xiaoye97-BepInEx-5.4.17"]:
            raise ValueError("Unexpected dependency contract")
        changelog = archive.read("CHANGELOG.md").decode("utf-8")
        heading = re.search(r"^## v?(\d+\.\d+\.\d+)\s*$", changelog, re.MULTILINE)
        if not heading or heading[1] != version:
            raise ValueError("Manifest/changelog versions differ")
        png = archive.read("icon.png")
        if png[:8] != b"\x89PNG\r\n\x1a\n" or png[12:16] != b"IHDR" or struct.unpack(">II", png[16:24]) != (256, 256):
            raise ValueError("Icon must be a 256x256 PNG")
        records = json.loads(archive.read("SHA256SUMS.json"))
        if {item["File"] for item in records} != expected - {"SHA256SUMS.json"} or len(records) != len(expected) - 1:
            raise ValueError("Checksum coverage differs from payload")
        for item in records:
            content = archive.read(item["File"])
            if len(content) != item["Bytes"] or hashlib.sha256(content).hexdigest().upper() != item["SHA256"]:
                raise ValueError(f"Checksum mismatch: {item['File']}")
        runtime = archive.read("nvngx_dlss.dll")
        if hashlib.sha256(runtime).hexdigest().upper() != "3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983":
            raise ValueError("Package contains an unexpected or development NGX runtime")
        amd_pins = {
            "amd_fidelityfx_loader_dx12.dll": "E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA",
            "amd_fidelityfx_upscaler_dx12.dll": "D0DCCCC74A43C44BA435B7A369B456E0970D8A4464E4BD683119B374F2C9FB46",
            "AMD-FSR-SDK-LICENSE.md": "F0DA09D71AD5C82759A179E774535D4A829E5C96C49294167C1152402B2CB400",
        }
        for name, digest in amd_pins.items():
            if hashlib.sha256(archive.read(name)).hexdigest().upper() != digest:
                raise ValueError(f"Unexpected AMD runtime or incomplete license: {name}")
        for name in ("README.md", "LICENSE", "third-party.md", "NVIDIA-RTX-SDK-LICENSE.txt", "NVIDIA-DLSS-NOTICES.txt", "AMD-FSR-SDK-LICENSE.md"):
            archive.read(name).decode("utf-8")
    print(f"Validated {manifest['name']} {version}: {len(expected)} flat-root files, metadata, icon, CRCs and payload hashes.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: validate-package.py <package.zip>")
    validate(sys.argv[1])
