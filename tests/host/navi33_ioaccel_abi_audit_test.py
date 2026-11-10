#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Synthetic Cobalt64 model tests. No Apple binaries or private symbols."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import navi33_ioaccel_abi_audit as abi

def fixture():
    return {"classes": {
        name: {"nslots": val["nslots"], "size": val.get("size"),
               "rows": [{"i": i, "kind": "inherit"} for i in range(val["nslots"])]}
        for name, val in abi.REFERENCE.items()
    }}

class AbiAuditTests(unittest.TestCase):
    def test_expected_model(self):
        self.assertEqual(abi.audit_model(fixture()), [])
    def test_size_mismatch(self):
        m = fixture()
        m["classes"]["IOGraphicsAccelerator2"]["size"] = 4000
        self.assertTrue(any("instance size" in s for s in abi.audit_model(m)))
    def test_slot_mismatch(self):
        m = fixture()
        m["classes"]["IOAccelSurface"]["nslots"] = 330
        self.assertTrue(any("slot count" in s for s in abi.audit_model(m)))
    def test_slot_order_mismatch(self):
        m = fixture()
        m["classes"]["IOAccelDevice2"]["rows"][1]["i"] = 2
        self.assertTrue(any("reordered" in s for s in abi.audit_model(m)))
    def test_missing_class(self):
        m = fixture()
        del m["classes"]["IOAccelDisplayPipe"]
        self.assertTrue(any("missing class" in s for s in abi.audit_model(m)))
    def test_bad_schema(self):
        self.assertTrue(abi.audit_model({"classes": []}))
    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "model.json"
            command = [sys.executable, str(ROOT / "tools" / "navi33_ioaccel_abi_audit.py"),
                       "--model", str(path)]
            path.write_text(json.dumps(fixture()), encoding="utf-8")
            ok = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            bad = fixture()
            bad["classes"]["IOAccelSurface"]["nslots"] = 330
            path.write_text(json.dumps(bad), encoding="utf-8")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)

if __name__ == "__main__":
    unittest.main()
