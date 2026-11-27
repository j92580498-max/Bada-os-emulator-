import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path


CLI = Path(sys.argv.pop(1)).resolve()
APPS_CIPHER = Path(__file__).parent / "fixtures" / "apps-reset.hex"


def lcd_rc2():
    offset, width, height = 0x41000, 240, 400
    frame = (struct.pack("<H", 0xF800) * (width * height // 2) +
             struct.pack("<H", 0x001F) * (width * height // 2))
    resource = bytearray(offset)
    struct.pack_into("<4I", resource, 0x40000, 0x5A5B5C5D, 0x40000, offset, len(frame))
    return resource + frame


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


def apps_image():
    # Synthetic reset stub at 0x81000480. The encrypted blocks use zero
    # initial round keys and Samsung's ciphertext chaining.
    encrypted = bytes.fromhex(APPS_CIPHER.read_text(encoding="ascii"))
    footer = bytearray(1024)
    for offset, word in {
        0x00: 0xABCDABCD,
        0x04: 0x00A00000,
        0x48: 0x79461379,
        0x190: 0x79461379,
        0x194: len(encrypted),
    }.items():
        struct.pack_into("<I", footer, offset, word)
    return encrypted + footer


def tcm_image():
    data = bytearray(0x260)
    struct.pack_into("<I", data, 0x00, 0xE59FF024)
    struct.pack_into("<I", data, 0x20, 0xBABEFACE)
    struct.pack_into("<I", data, 0x2C, 0x803001FC)
    words = [
        0xE59F0040,  # Load reset-register address from +0x244.
        0xE3A01000,
        0xE5801000,  # The known boot write at +0x204.
        0xE59F0038,
        0xEE090F11,  # Configure DTCM.
        0xEE192F11,  # Read the DTCM configuration back in r2.
        0xE59F3030,
        0xE3A04033,
        0xE5834000,  # Write through the DTCM mapping.
        0xE5935000,  # Read through the DTCM mapping.
        0xE59F0024,
        0xEE090F31,  # Configure ITCM at zero.
        0xE3A06000,
        0xE5865000,
        0xE5967000,  # Read through the ITCM mapping.
        0xE59F8014,
        0xE5989000,  # A different unmodeled access still stops the probe.
        0xEAFFFFFE,
        0x08880048,
        0x08700019,
        0x08700000,
        0x0000001D,
        0x08810000,
    ]
    struct.pack_into(f"<{len(words)}I", data, 0x1FC, *words)
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
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result.stdout if status == 0 else result.stderr

    def test_inventory_and_arm_fault(self):
        self.write({"firmware/AMSS.BIN": amss_image(), "Rsrc_test.rc1": b"QMD\x00",
                    "apps_compressed.bin": b"unused"})
        report = self.execute("inspect")
        for text in ("ARM reset vector", "QMD", "APPS", "No recognized standalone bootloader"):
            self.assertIn(text, report)
        report = self.execute("trace")
        self.assertIn("QEMU ARM1176", report)
        self.assertIn("unmapped read at 0x08810000 (4 bytes)", report)
        self.assertIn("PC 0x80300048", report)
        self.assertIn("r0=0x0000002a", report)

    def test_thumb_and_budget(self):
        self.write({"amss.bin": amss_image(thumb=True)})
        report = self.execute("trace", "12")
        self.assertIn("Instruction limit: 12", report)
        self.assertIn("Stop: instruction limit", report)
        self.assertIn("r0=0x0000002a", report)

    def test_apps_decrypts_and_executes(self):
        self.write({"images/APPS_COMPRESSED.BIN": apps_image()})
        report = self.execute("trace-apps")
        self.assertIn("APPS reset trace (QEMU ARM1176)", report)
        self.assertIn("Vector 0x81000000 -> 0x81000480", report)
        self.assertIn("unimplemented RAMCTRL setting at 0x08880048 (4 bytes)", report)
        self.assertIn("Write value 0x0000002b", report)
        self.assertIn("r2=0x08700015", report)
        self.assertIn("r3=0xffff0015", report)

    def test_guest_draws_rc2_bitmap(self):
        self.write({"Resources (Low).rc2": lcd_rc2()})
        image = self.path.with_suffix(".png")
        self.assertIn("Saved QEMU LCD capture", self.execute("boot-logo", str(image)))
        data = image.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack_from(">IIBB", data, 16), (240, 400, 8, 2))
        chunks = []
        position = 8
        while position < len(data):
            length, kind = struct.unpack_from(">I4s", data, position)
            if kind == b"IDAT":
                chunks.append(data[position + 8:position + 8 + length])
            position += length + 12
        decoded = zlib.decompress(b"".join(chunks))
        self.assertEqual(decoded, (b"\x00" + bytes((255, 0, 0)) * 240) * 200 +
                                  (b"\x00" + bytes((0, 0, 255)) * 240) * 200)

    def test_tcm_and_unmapped_stop(self):
        self.write({"amss.bin": tcm_image()})
        report = self.execute("trace", "128")
        self.assertIn("unmapped read at 0x08810000", report)
        self.assertIn("r2=0x08700019", report)
        self.assertIn("r5=0x00000033", report)
        self.assertIn("r7=0x00000033", report)

    def test_diagnostic_stack_ram(self):
        image = tcm_image()
        struct.pack_into("<I", image, 0x254, 0x0870DDA0)
        self.write({"amss.bin": image})
        self.assertIn("unmapped read at 0x0870dda0", self.execute("trace", "128"))
        report = self.execute("trace", "128", "--diagnostic")
        self.assertIn("AMSS stack RAM mapped", report)
        self.assertIn("RAMCTRL cleared", report)
        self.assertIn("Stop: instruction limit", report)

    def test_unsupported_inputs(self):
        self.path.write_bytes(b"not a zip")
        self.assertIn("Could not read the firmware ZIP", self.execute("inspect", status=1))
        self.write({"apps_compressed.bin": b"unused"})
        self.assertIn("does not contain amss.bin", self.execute("trace", status=1))
        self.assertIn("APPS image size", self.execute("trace-apps", status=1))
        invalid = bytearray(apps_image())
        invalid[-1024] = 0
        self.write({"apps_compressed.bin": invalid})
        self.assertIn("Unsupported APPS footer", self.execute("trace-apps", status=1))
        self.write({"amss.bin": bytes(256)})
        self.assertIn("Unsupported AMSS reset header", self.execute("trace", status=1))
        self.write({"amss.bin": amss_image(), "copy/amss.bin": amss_image()})
        self.assertIn("multiple amss.bin", self.execute("trace", status=1))
        self.assertIn("Instruction limit", self.execute("trace", "0", status=1))


if __name__ == "__main__":
    unittest.main()
