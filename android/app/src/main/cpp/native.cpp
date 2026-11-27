#include "wave723/firmware.hpp"

#include <jni.h>

extern "C" JNIEXPORT jstring JNICALL
Java_org_wave723_emulator_Native_readFirmware(JNIEnv* env, jclass, jstring path,
                                             jstring engine, jboolean trace) {
    const char* chars = env->GetStringUTFChars(path, nullptr);
    if (!chars) return nullptr;
    const char* executable = env->GetStringUTFChars(engine, nullptr);
    if (!executable) {
        env->ReleaseStringUTFChars(path, chars);
        return nullptr;
    }
    try {
        const auto result = trace ? wave723::traceReset(chars, wave723::defaultInstructionLimit, executable)
                                  : wave723::inspectFirmware(chars);
        env->ReleaseStringUTFChars(path, chars);
        env->ReleaseStringUTFChars(engine, executable);
        return env->NewStringUTF(result.c_str());
    } catch (const std::exception& error) {
        env->ReleaseStringUTFChars(path, chars);
        env->ReleaseStringUTFChars(engine, executable);
        const auto type = env->FindClass("java/io/IOException");
        if (type) env->ThrowNew(type, error.what());
        return nullptr;
    }
}
