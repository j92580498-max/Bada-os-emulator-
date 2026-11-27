import os
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

    def test_stock_boot_requires_a_guest_frame(self):
        self.write({"apps_compressed.bin": apps_image(), "Resources.rc2": lcd_rc2()})
        image = self.path.with_suffix(".png")
        self.assertIn("Stock APPS did not produce a visible LCD frame",
                      self.execute("boot", str(image), "128", status=1))
        self.assertFalse(image.exists())

    def test_xxkg1_boot_provisions_the_factory_flash(self):
        def component(start, payload=b"test payload"):
            footer = bytearray(1024)
            struct.pack_into("<II", footer, 0, 0xABCDABCD, start)
            return payload + footer

        apps = bytearray(apps_image())
        apps[-1024 + 0x1e8:-1024 + 0x1f4] = b"S723E+XX+KG1"
        files = {"apps_compressed.bin": apps, "amss.bin": component(0x00200000, amss_image()),
                 "sample.rc1": component(0x04100000), "sample.rc2": component(0x1eb00000),
                 "sample.fota": component(0x08a00000)}
        self.write(files)
        png = self.path.with_suffix(".png")
        result = subprocess.run([str(CLI), "boot", str(self.path), str(png), "128"],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("XXKG1 FSR volume and flash components attached", result.stdout)
        self.assertIn("r2=0x08700015", result.stdout)
        self.assertFalse(png.exists())
        files["sample.rc1"] = component(0x00200000)
        self.write(files)
        self.assertIn("Unexpected XXKG1 flash footer for RSRC1",
                      self.execute("boot", str(png), "128", status=1))

    def test_modem_starts_from_mailbox_and_has_private_tcm(self):
        # (register, value) emits an ARM literal load. Other entries are opcodes.
        def program(instructions):
            words = list(instructions)
            for i, instruction in enumerate(instructions):
                if isinstance(instruction, tuple):
                    reg, value = instruction
                    words[i] = 0xE59F0000 | (reg << 12) | (4 * (len(words) - i - 2))
                    words.append(value)
            return struct.pack(f"<{len(words)}I", *words)

        ap, cp = self.path.with_name("ap.bin"), self.path.with_name("cp.bin")
        ap.write_bytes(program([
            (0, 0x08700015), 0xEE090F11, (1, 0x08700000), (2, 0x11), 0xE5812000,
            (3, 0xffff0020), (4, 0x80300000), 0xE5834000,  # Publish modem reset vector
            (3, 0x08810038), (4, 6), 0xE5834000,           # Release modem
            (3, 0x800b4000), 0xE5935000, 0xE3550022, 0x1AFFFFFC,  # Poll shared RAM
            0xE5916000, (3, 0x0bfffffc), 0xE5937000,     # Check local DTCM and stop
        ]))
        cp.write_bytes(program([
            (0, 0x08700019), 0xEE090F11, (1, 0x08700000), (2, 0x22), 0xE5812000,
            (3, 0x800b4000), 0xE5812000, 0xE5914000, 0xE5834000, 0xEAFFFFFE,
        ]))
        engine = CLI.with_name("qemu-system-arm.exe" if os.name == "nt" else "qemu-system-arm")
        result = subprocess.run(
            [os.environ.get("WAVE723_QEMU", str(engine)), "-M", "wave723", "-m", "32M",
             "-smp", "2", "-accel", "tcg,thread=single", "-icount", "shift=0,sleep=off",
             "-nodefaults", "-display", "none", "-monitor", "none", "-serial", "none",
             "-kernel", str(ap), "-initrd", str(cp), "-append", "diagnostic:apps:2000000"],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("AMSS released at 0x80300000", result.stdout)
        self.assertIn("unmapped read at 0x0bfffffc", result.stdout)
        self.assertIn("r5=0x00000022", result.stdout)
        self.assertIn("r6=0x00000011", result.stdout)

    def test_tcm_and_unmapped_stop(self):
        self.write({"amss.bin": tcm_image()})
        report = self.execute("trace", "128")
        self.assertIn("unmapped read at 0x08810000", report)
        self.assertIn("r2=0x08700019", report)
        self.assertIn("r5=0x00000033", report)
        self.assertIn("r7=0x00000033", report)

    def test_onenand_reads_the_selected_page(self):
        image = self.path.with_name("nand-test.bin")
        flash = self.path.with_name("flash.bin")
        spare = self.path.with_name("spare.bin")
        image.write_bytes(struct.pack(
            "<18I",
            0xE59F002C, 0xE59F102C,       # Load OneNAND base and START_ADDRESS8.
            0xE3A02004, 0xE1C120B0,       # Select page 1.
            0xE59F1024, 0xE3A02000, 0xE1C120B0,  # Issue READ.
            0xE59F101C, 0xE1D130B0,       # Read completion status.
            0xE5904400, 0xE59F1014, 0xE5915000,  # Read DataRAM and SpareRAM.
            0xEAFFFFFE,
            0x00400000, 0x0041E20E, 0x0041E440, 0x0041E482, 0x00410020,
        ))
        flash.write_bytes(b"\xff" * 4096 +
                          struct.pack("<I", 0x12345678) + b"\xff" * (3 * 4096 - 4))
        spare.write_bytes(b"\xff" * 128 + struct.pack("<I", 0xA5A5FFFF) + b"\xff" * 380)
        engine = CLI.with_name("qemu-system-arm.exe" if os.name == "nt" else "qemu-system-arm")
        drive = "if=none,id=wave723-flash,format=raw,readonly=on,file=" + str(flash).replace(",", ",,")
        result = subprocess.run(
            [os.environ.get("WAVE723_QEMU", str(engine)), "-M", "wave723", "-m", "32M",
             "-accel", "tcg,thread=single,one-insn-per-tb=on", "-icount", "shift=0,sleep=off",
             "-nodefaults", "-display", "none", "-monitor", "none", "-serial", "none",
             "-kernel", str(image), "-append", "amss:128", "-drive", drive,
             "-drive", "if=none,id=wave723-spare,format=raw,readonly=on,file=" +
             str(spare).replace(",", ",,")],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Stop: instruction limit", result.stdout)
        self.assertIn("r3=0x00008080", result.stdout)
        self.assertIn("r4=0x12345678", result.stdout)
        self.assertIn("r5=0xa5a5ffff", result.stdout)

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
