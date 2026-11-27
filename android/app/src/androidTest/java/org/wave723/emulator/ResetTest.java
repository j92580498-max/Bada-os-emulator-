package org.wave723.emulator;

import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class ResetTest {
    @Test
    public void readsZipAndExecutesArmThroughJni() throws Exception {
        File zip = File.createTempFile("test-wave723-", ".zip",
                InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir());
        try {
            ByteBuffer image = ByteBuffer.allocate(256).order(ByteOrder.LITTLE_ENDIAN);
            image.putInt(0x00, 0xe59ff024);
            image.putInt(0x20, 0xbabeface);
            image.putInt(0x2c, 0x80300040);
            image.putInt(0x40, 0xe3a0002a);
            image.putInt(0x44, 0xeaFFFFFE);
            try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(zip))) {
                out.putNextEntry(new ZipEntry("amss.bin"));
                out.write(image.array());
                out.closeEntry();
            }
            String inventory = Native.readFirmware(zip.getAbsolutePath(), false);
            assertTrue(inventory, inventory.contains("ARM reset vector"));
            String trace = Native.readFirmware(zip.getAbsolutePath(), true);
            assertTrue(trace, trace.contains("r0=0x0000002a"));
            assertTrue(trace, trace.contains("Stop: instruction limit"));
        } finally {
            zip.delete();
        }
    }
}
