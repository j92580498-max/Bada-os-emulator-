import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


CLI = Path(sys.argv.pop(1)).resolve()


def amss_image(thumb=False):
    data = bytearray(0x100)
    for offset, word in {
        0x00: 0xE59FF024,  # ldr pc, [pc, #0x24]
        0x20: 0xBABEFACE,
        0x2C: 0x80300040,
    }.items():
        struct.pack_into("<I", data, offset, word)
    if thumb:
        struct.pack_into("<IIHH", data, 0x40, 0xE28F1001, 0xE12FFF11, 0x202A, 0xE7FE)
    else:
        struct.pack_into("<5I", data, 0x40,
                         0xE3A0002A, 0xE59F1004, 0xE5912000, 0xEAFFFFFE, 0x08810000)
    return data


class Smoke(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "firmware test.zip"

    def write(self, entries):
        with zipfile.ZipFile(self.path, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, image in entries.items():
                archive.writestr(name, image)

    def execute(self, action, *args, status=0):
        result = subprocess.run([str(CLI), action, str(self.path), *args],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result.stdout if status == 0 else result.stderr

    def test_inventory_and_arm_fault(self):
        self.write({"firmware/AMSS.BIN": amss_image(), "Rsrc_test.rc1": b"QMD\x00",
                    "apps_compressed.bin": b"unused"})
        report = self.execute("inspect")
        for text in ("ARM reset vector", "QMD", "APPS", "No recognized standalone bootloader"):
            self.assertIn(text, report)
        report = self.execute("trace")
        self.assertIn("Instructions reached: 4 /", report)
        self.assertIn("unmapped read at 0x08810000 (4 bytes)", report)
        self.assertIn("PC 0x80300048", report)
        self.assertIn("r0=0x0000002a", report)

    def test_thumb_and_budget(self):
        self.write({"amss.bin": amss_image(thumb=True)})
        report = self.execute("trace", "12")
        self.assertIn("Instructions reached: 12 / 12", report)
        self.assertIn("Stop: instruction limit", report)
        self.assertIn("r0=0x0000002a", report)

    def test_unsupported_inputs(self):
        self.path.write_bytes(b"not a zip")
        self.assertIn("Could not read the firmware ZIP", self.execute("inspect", status=1))
        self.write({"apps_compressed.bin": b"unused"})
        self.assertIn("does not contain amss.bin", self.execute("trace", status=1))
        self.write({"amss.bin": bytes(256)})
        self.assertIn("Unsupported AMSS reset header", self.execute("trace", status=1))
        self.write({"amss.bin": amss_image(), "copy/amss.bin": amss_image()})
        self.assertIn("multiple amss.bin", self.execute("trace", status=1))
        self.assertIn("Instruction limit", self.execute("trace", "0", status=1))


if __name__ == "__main__":
    unittest.main()
