#!/usr/bin/env python3
"""Build a Sequoia installer-test EFI for ASUS TUF B450M-PLUS GAMING / Ryzen 5 3600.
The RX 7600 is unsupported by stock macOS. No RedNavi firmware writes are included.
"""
import io
import os
import pathlib
import plistlib
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import uuid
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT / "output"
EFI = OUT / "EFI"
CACHE = ROOT / "cache"
OC_VER = "1.0.8"
KEXTS = [
    ("Lilu", "acidanthera/Lilu", "1.7.2", "Lilu-1.7.2-RELEASE.zip"),
    ("VirtualSMC", "acidanthera/VirtualSMC", "1.3.8", "VirtualSMC-1.3.8-RELEASE.zip"),
    ("WhateverGreen", "acidanthera/WhateverGreen", "1.7.1", "WhateverGreen-1.7.1-RELEASE.zip"),
    ("RealtekRTL8111", "Mieze/RTL8111_driver_for_OS_X", "v3.0.0", "RealtekRTL8111-V3.0.0.zip"),
    ("AppleALC", "acidanthera/AppleALC", "1.9.8", "AppleALC-1.9.8-RELEASE.zip"),
]

def download(url, name):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if path.exists() and path.stat().st_size > 100:
        return path.read_bytes()
    print("Downloading", name, flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "Sequoia-EFI-GitHub-Actions"})
    with urllib.request.urlopen(request, timeout=180) as response:
        data = response.read()
    if len(data) < 100:
        raise RuntimeError("Empty or invalid download " + url)
    path.write_bytes(data)
    return data

def member(zf, suffix):
    found = [n for n in zf.namelist() if n.replace("\\", "/").endswith(suffix)]
    if not found:
        raise RuntimeError("Missing ZIP entry: " + suffix)
    return min(found, key=len)

def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)

def extract_kext(blob, name):
    with zipfile.ZipFile(io.BytesIO(blob)) as zf:
        info = member(zf, name + ".kext/Contents/Info.plist")
        prefix = info[:-len("Contents/Info.plist")]
        for n in zf.namelist():
            if not n.startswith(prefix) or n.endswith("/"):
                continue
            rel = pathlib.PurePosixPath(n[len(prefix):])
            if rel.is_absolute() or ".." in rel.parts:
                raise RuntimeError("Unsafe ZIP path")
            put(EFI / "OC" / "Kexts" / (name + ".kext") / pathlib.Path(*rel.parts), zf.read(n))

def kext_entry(name):
    base = EFI / "OC" / "Kexts" / (name + ".kext")
    with (base / "Contents" / "Info.plist").open("rb") as f:
        meta = plistlib.load(f)
    executable = meta.get("CFBundleExecutable", "")
    entry = {
        "Arch": "x86_64",
        "BundlePath": name + ".kext",
        "Comment": "Official pinned release",
        "Enabled": True,
        "ExecutablePath": "Contents/MacOS/" + executable if executable else "",
        "MaxKernel": "",
        "MinKernel": "",
        "PlistPath": "Contents/Info.plist",
    }
    if executable and not (base / entry["ExecutablePath"]).is_file():
        raise RuntimeError("Kext executable missing: " + name)
    return entry

def main():
    oc_bytes = download(
        "https://github.com/acidanthera/OpenCorePkg/releases/download/1.0.8/OpenCore-1.0.8-RELEASE.zip",
        "OpenCore-1.0.8-RELEASE.zip"
    )
    kext_bytes = {
        name: download(f"https://github.com/{repo}/releases/download/{tag}/{filename}", filename)
        for name, repo, tag, filename in KEXTS
    }
    patches_bytes = download("https://raw.githubusercontent.com/AMD-OSX/AMD_Vanilla/master/patches.plist", "amd-vanilla-patches.plist")
    aml = download("https://raw.githubusercontent.com/dortania/Getting-Started-With-ACPI/master/extra-files/compiled/SSDT-EC-USBX-DESKTOP.aml", "SSDT-EC-USBX-DESKTOP.aml")
    hfs = download("https://raw.githubusercontent.com/acidanthera/OcBinaryData/master/Drivers/HfsPlus.efi", "HfsPlus.efi")
    if not aml.startswith(b"SSDT") or not hfs.startswith(b"MZ"):
        raise RuntimeError("ACPI or HFS binary failed signature check")
    if OUT.exists():
        shutil.rmtree(OUT)
    EFI.mkdir(parents=True)

    with zipfile.ZipFile(io.BytesIO(oc_bytes)) as oc:
        put(EFI / "BOOT" / "BOOTx64.efi", oc.read(member(oc, "X64/EFI/BOOT/BOOTx64.efi")))
        put(EFI / "OC" / "OpenCore.efi", oc.read(member(oc, "X64/EFI/OC/OpenCore.efi")))
        put(EFI / "OC" / "Drivers" / "OpenRuntime.efi", oc.read(member(oc, "X64/EFI/OC/Drivers/OpenRuntime.efi")))
        put(EFI / "OC" / "Drivers" / "HfsPlus.efi", hfs)
        put(EFI / "OC" / "ACPI" / "SSDT-EC-USBX-DESKTOP.aml", aml)
        for name, data in kext_bytes.items():
            extract_kext(data, name)

        config = plistlib.loads(oc.read(member(oc, "Docs/Sample.plist")))
        patches = plistlib.loads(patches_bytes)["Kernel"]["Patch"]
        count = 0
        for patch in patches:
            if "Force cpuid_cores_per_package" in patch.get("Comment", ""):
                replace = bytearray(patch["Replace"])
                if len(replace) < 2 or replace[0] not in (0xB8, 0xBA):
                    raise RuntimeError("Unexpected AMD patch data")
                replace[1] = 6
                patch["Replace"] = bytes(replace)
                count += 1
        if count != 4:
            raise RuntimeError("Expected four AMD Vanilla physical core patches; got " + str(count))

        config["ACPI"]["Add"] = [{
            "Comment": "Desktop EC/USBX",
            "Enabled": True,
            "Path": "SSDT-EC-USBX-DESKTOP.aml"
        }]
        config["ACPI"]["Delete"] = []
        config["ACPI"]["Patch"] = []
        config["Booter"]["MmioWhitelist"] = []
        config["Booter"]["Patch"] = []
        config["Booter"]["Quirks"].update({
            "AvoidRuntimeDefrag": True,
            "DevirtualiseMmio": False,
            "EnableWriteUnprotector": False,
            "RebuildAppleMemoryMap": True,
            "ResizeAppleGpuBars": -1,
            "SetupVirtualMap": True,
            "SyncRuntimePermissions": True,
            "ProtectUefiServices": False,
        })
        config["DeviceProperties"]["Add"] = {}
        config["DeviceProperties"]["Delete"] = {}
        config["Kernel"]["Block"] = []
        config["Kernel"]["Force"] = []
        config["Kernel"]["Patch"] = patches
        config["Kernel"]["Emulate"]["DummyPowerManagement"] = True
        config["Kernel"]["Quirks"].update({
            "DisableLinkeditJettison": True,
            "PanicNoKextDump": True,
            "PowerTimeoutKernelPanic": True,
            "ProvideCurrentCpuInfo": True,
            "XhciPortLimit": False,
            "AppleCpuPmCfgLock": False,
            "AppleXcpmCfgLock": False,
            "AppleXcpmExtraMsrs": False,
            "DisableIoMapper": True,
        })
        config["Kernel"]["Add"] = [kext_entry(name) for name, *_ in KEXTS]
        config["Misc"]["Boot"].update({
            "HideAuxiliary": False, "PickerMode": "Builtin",
            "ShowPicker": True, "Timeout": 10
        })
        config["Misc"]["Debug"].update({
            "AppleDebug": True, "ApplePanic": True, "DisableWatchDog": True, "Target": 3
        })
        config["Misc"]["Security"].update({
            "AllowSetDefault": True, "DmgLoading": "Signed",
            "ScanPolicy": 0, "SecureBootModel": "Disabled", "Vault": "Optional"
        })
        config["Misc"]["Tools"] = []

        nv = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
        config["NVRAM"]["Add"][nv] = {
            "boot-args": "-v keepsyms=1 debug=0x100 -radvesa alcid=1",
            "csr-active-config": bytes([0, 0, 0, 0]),
            "prev-lang:kbd": b"en-US:0"
        }
        config["NVRAM"]["Delete"][nv] = ["boot-args", "csr-active-config", "prev-lang:kbd"]
        serial = ""
        mlb = ""
        serial_candidates = [n for n in oc.namelist() if n.lower().endswith("/macserial.exe")]
        if serial_candidates:
            with tempfile.TemporaryDirectory() as tmp:
                macserial = pathlib.Path(tmp) / "macserial.exe"
                put(macserial, oc.read(serial_candidates[0]))
                cp = subprocess.run([str(macserial), "-m", "iMacPro1,1", "-n", "1"],
                                    capture_output=True, text=True, timeout=30)
                for line in cp.stdout.splitlines():
                    m = re.search(r"\b([A-Z0-9]{11,12})\s*\|\s*([A-Z0-9]{17})\b", line)
                    if m:
                        serial, mlb = m.groups()
                        break
        if not serial or not mlb:
            raise RuntimeError("macserial failed: refuse to publish invalid SMBIOS IDs")
        config["PlatformInfo"]["Automatic"] = True
        config["PlatformInfo"]["UpdateSMBIOSMode"] = "Create"
        config["PlatformInfo"]["UpdateDataHub"] = True
        config["PlatformInfo"]["UpdateNVRAM"] = True
        config["PlatformInfo"]["UpdateSMBIOS"] = True
        config["PlatformInfo"]["Generic"].update({
            "SystemProductName": "iMacPro1,1",
            "SystemSerialNumber": serial,
            "MLB": mlb,
            "SystemUUID": str(uuid.uuid4()).upper(),
            "ROM": secrets.token_bytes(6),
            "ProcessorType": 0
        })
        config["UEFI"]["APFS"]["MinDate"] = -1
        config["UEFI"]["APFS"]["MinVersion"] = -1
        config["UEFI"]["Drivers"] = [
            {"Arguments": "", "Comment": "OpenCore Runtime", "Enabled": True,
             "HideVerbose": False, "LoadEarly": False, "Path": "OpenRuntime.efi"},
            {"Arguments": "", "Comment": "HFS+ Recovery", "Enabled": True,
             "HideVerbose": False, "LoadEarly": False, "Path": "HfsPlus.efi"}
        ]
        config["UEFI"]["Output"]["ProvideConsoleGop"] = True
        config["UEFI"]["Quirks"]["RequestBootVarRouting"] = True
        put(EFI / "OC" / "config.plist", plistlib.dumps(config, fmt=plistlib.FMT_XML, sort_keys=False))

        for driver in config["UEFI"]["Drivers"]:
            assert (EFI / "OC" / "Drivers" / driver["Path"]).is_file()
        for item in config["Kernel"]["Add"]:
            base = EFI / "OC" / "Kexts" / item["BundlePath"]
            assert (base / item["PlistPath"]).is_file()

        validate_matches = [n for n in oc.namelist() if n.lower().endswith("/ocvalidate.exe")]
        if not validate_matches:
            raise RuntimeError("OpenCore 1.0.8 ocvalidate.exe not present in official archive")
        with tempfile.TemporaryDirectory() as tmp:
            executable = pathlib.Path(tmp) / "ocvalidate.exe"
            put(executable, oc.read(validate_matches[0]))
            check = subprocess.run([str(executable), str(EFI / "OC" / "config.plist")],
                                   capture_output=True, text=True, timeout=90)
            print(check.stdout)
            print(check.stderr)
            if check.returncode != 0:
                raise RuntimeError("ocvalidate failed; do not publish this EFI")

    archive = OUT / "EFI-Sequoia-TUF-B450M.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for path in sorted(EFI.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(OUT).as_posix())
    (OUT / "README.txt").write_text(
        "Experimental Sequoia USB installer-test EFI. RX 7600 is unsupported by stock macOS.\n"
        "Do not erase Windows. Do not boot experimental RedNavi firmware-loading kexts.\n"
        "Generated SMBIOS IDs are for boot testing only; regenerate for long-term use.\n",
        encoding="utf-8"
    )
    print("BUILT", archive, flush=True)

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("BUILD FAILED:", e, file=sys.stderr)
        raise SystemExit(1)
