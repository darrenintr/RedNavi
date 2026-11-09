#!/usr/bin/env python3
"""Package a USB-ready FAT32 folder layout with official macOS Sequoia Recovery."""
import io
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent
EFI_ZIP = ROOT / "output" / "EFI-Sequoia-TUF-B450M.zip"
OC_ZIP = ROOT / "cache" / "OpenCore-1.0.8-RELEASE.zip"
OUT_ZIP = ROOT / "output" / "USB-Sequoia-Recovery-TUF-B450M.zip"

def main():
    assert EFI_ZIP.is_file(), "Build EFI first"
    assert OC_ZIP.is_file(), "OpenCore official archive missing"
    with zipfile.ZipFile(OC_ZIP) as z:
        names = [n for n in z.namelist() if n.endswith("Utilities/macrecovery/macrecovery.py")]
        assert names, "macrecovery.py missing"
        script = z.read(names[0])
        python_dir = names[0].rsplit("/", 1)[0] + "/"
        ancillary = {n[len(python_dir):]:z.read(n) for n in z.namelist()
                     if n.startswith(python_dir) and n.endswith(".py") and "/" not in n[len(python_dir):]}

    with tempfile.TemporaryDirectory() as temp:
        p = pathlib.Path(temp)
        (p / "macrecovery.py").write_bytes(script)
        for n, b in ancillary.items():
            (p / n).write_bytes(b)
        # Explicit Sequoia board from OpenCorePkg/recovery_urls.txt, not "latest" (Tahoe).
        cmd = [sys.executable, "macrecovery.py", "-b", "Mac-7BA5B2D9E42DDD94",
               "-m", "00000000000000000", "download"]
        print("Downloading official macOS Sequoia Recovery using:", " ".join(cmd),flush=True)
        subprocess.run(cmd, cwd=p, check=True, timeout=1200)
        candidates = list(p.glob("*.dmg"))
        chunklists = list(p.glob("*.chunklist"))
        assert len(candidates) == 1 and len(chunklists) == 1,             "Expected one .dmg and one .chunklist"
        dmg = candidates[0]
        chunk = chunklists[0]
        assert dmg.stat().st_size > 100 * 1024 * 1024
        assert chunk.stat().st_size > 100
        print(f"Recovery: {dmg.name} {dmg.stat().st_size} bytes; {chunk.name}",flush=True)

        with zipfile.ZipFile(EFI_ZIP) as ez:
            entries = [n for n in ez.namelist() if not n.endswith("/")]
            assert "EFI/BOOT/BOOTx64.efi" in entries
            assert "EFI/OC/config.plist" in entries

            OUT_ZIP.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(OUT_ZIP, "w", compression=zipfile.ZIP_DEFLATED,
                                 compresslevel=3, allowZip64=True) as z:
                for entry in entries:
                    z.writestr(entry, ez.read(entry))
                z.write(dmg, "com.apple.recovery.boot/" + dmg.name,
                        compress_type=zipfile.ZIP_STORED)
                z.write(chunk, "com.apple.recovery.boot/" + chunk.name,
                        compress_type=zipfile.ZIP_STORED)
                z.writestr("README-USB.txt",
                    "ASUS TUF B450M-PLUS GAMING / Ryzen 5 3600 / RX 7600\n"
                    "USB Recovery only; internet needed during installation.\n"
                    "Extract everything to ROOT of a FAT32/GPT USB volume; do not extract the"
                    " ZIP as a nested folder.\n"
                    "RX 7600 has NO verified native Sequoia display support.\n"
                    "Do not erase your existing Windows SSD.\n")
        with zipfile.ZipFile(OUT_ZIP) as verify:
            assert verify.testzip() is None, "ZIP CRC check failed"
            actual = set(verify.namelist())
            assert "EFI/OC/config.plist" in actual
            assert "com.apple.recovery.boot/" + dmg.name in actual
            assert "com.apple.recovery.boot/" + chunk.name in actual
        print("SUCCESS:",OUT_ZIP,OUT_ZIP.stat().st_size,"bytes",flush=True)

if __name__ == "__main__":
    main()
