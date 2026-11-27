package org.wave723.emulator;

final class Native {
    static {
        System.loadLibrary("wave723_android");
    }

    private Native() {}

    static native String readFirmware(String path, boolean trace);
}
