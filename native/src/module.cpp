#include <sys/types.h>
#include "zygisk.hpp"

#include <jni.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <thread>
#include <chrono>
#include <android/log.h>

#define TAG "QQEmoji"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Not declared in NDK public headers for all API levels.
extern "C" int __system_property_get(const char* name, char* value);

static JavaVM* g_vm = nullptr;

// --- ArtMethod field offsets (determined at runtime) ---
static uint32_t g_access_flags_offset = 4;  // Fixed at 4 on Android M+ (API 23+)
static uint32_t g_data_offset = 0;          // data_ / entry_point_from_jni_
static uint32_t g_entry_point_offset = 0;   // entry_point_from_quick_compiled_code_
static void* g_jni_trampoline = nullptr;    // art_quick_generic_jni_trampoline

// --- Access flag constants ---
// Source: art/modifiers.h in AOSP (stable across all versions)
static constexpr uint32_t kAccNative = 0x0100;
// Version-dependent flags
static uint32_t kAccCompileDontBother = 0;
static uint32_t kAccPreCompiled = 0;

// ============================================================
// Native callback functions — return -1 to force system emoji fallback
// ============================================================

static jint singleEmojiCallback(JNIEnv*, jobject, jint) {
    return -1;
}

static jint doubleEmojiCallback(JNIEnv*, jobject, jint, jint) {
    return -1;
}

// ============================================================
// Initialize version-dependent access flag values
// ============================================================

static void initAccessFlags(int api_level) {
    // kAccCompileDontBother value depends on Android version
    if (api_level >= 27) {
        kAccCompileDontBother = 0x02000000;  // O_MR1+
    } else if (api_level >= 24) {
        kAccCompileDontBother = 0x00080000;  // N
    }

    // kAccPreCompiled only on Android 11+
    if (api_level == 30) {
        kAccPreCompiled = 0x00200000;  // R
    } else if (api_level >= 31) {
        kAccPreCompiled = 0x01000000;  // S+
    }

    LOGI("API level=%d, kAccCompileDontBother=0x%x, kAccPreCompiled=0x%x",
         api_level, kAccCompileDontBother, kAccPreCompiled);
}

// ============================================================
// Determine ArtMethod field offsets AND JNI trampoline at runtime.
//
// Key insight: Object.hashCode() is a NATIVE method. Its ArtMethod's
// entry_point_from_compiled_code_ already points to
// art_quick_generic_jni_trampoline. We read it directly — no dlopen,
// no ELF parsing, no symbol resolution needed.
//
// Offset strategy (same as Pine/epic):
// 1. access_flags_ is at offset 4 (GcRoot<Class> is 4 bytes, then access_flags_)
// 2. sizeof(ArtMethod) = gap between adjacent methods
// 3. data_ and entry_point are the last two pointer-sized fields
// ============================================================

static bool determineOffsets(JNIEnv* env) {
    jclass objClass = env->FindClass("java/lang/Object");
    if (!objClass || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("Cannot find java.lang.Object");
        return false;
    }

    // Get hashCode — a native method whose entry_point IS the JNI trampoline
    jmethodID hashCodeMid = env->GetMethodID(objClass, "hashCode", "()I");
    jmethodID equalsMid = env->GetMethodID(objClass, "equals", "(Ljava/lang/Object;)Z");
    jmethodID toStringMid = env->GetMethodID(objClass, "toString", "()Ljava/lang/String;");

    if (env->ExceptionCheck() || !hashCodeMid || !equalsMid || !toStringMid) {
        env->ExceptionClear();
        LOGE("Cannot get Object methods");
        env->DeleteLocalRef(objClass);
        return false;
    }

    // Convert jmethodID → real ArtMethod* via reflection.
    // FromReflectedMethod reads Method.artMethod field directly — works on all
    // Android versions including R+ where jmethodID may be index-encoded.
    uintptr_t ptrs[3];
    jmethodID mids[3] = {hashCodeMid, equalsMid, toStringMid};
    for (int i = 0; i < 3; i++) {
        jobject ref = env->ToReflectedMethod(objClass, mids[i], JNI_FALSE);
        ptrs[i] = reinterpret_cast<uintptr_t>(env->FromReflectedMethod(ref));
        env->DeleteLocalRef(ref);
    }

    // Keep hashCode's ArtMethod pointer for trampoline extraction
    uintptr_t hashCodeArtMethod = ptrs[0];

    env->DeleteLocalRef(objClass);

    LOGI("hashCode ArtMethod @ %p", (void*)hashCodeArtMethod);

    // Sort to find adjacent method pairs → sizeof(ArtMethod)
    if (ptrs[0] > ptrs[1]) std::swap(ptrs[0], ptrs[1]);
    if (ptrs[1] > ptrs[2]) std::swap(ptrs[1], ptrs[2]);
    if (ptrs[0] > ptrs[1]) std::swap(ptrs[0], ptrs[1]);

    size_t gap1 = ptrs[1] - ptrs[0];
    size_t gap2 = ptrs[2] - ptrs[1];
    size_t artMethodSize = 0;
    if (gap1 > 0 && gap2 > 0) {
        artMethodSize = std::min(gap1, gap2);
    } else if (gap1 > 0) {
        artMethodSize = gap1;
    } else if (gap2 > 0) {
        artMethodSize = gap2;
    }

    if (artMethodSize < sizeof(void*) * 2 || artMethodSize > 128) {
        LOGE("Unusual ArtMethod size %zu, using fallback", artMethodSize);
        artMethodSize = (sizeof(void*) == 8) ? 32 : 24;
    }

    // PtrSizedFields are the last two pointer-sized fields:
    //   { void* data_, void* entry_point_from_quick_compiled_code_ }
    g_access_flags_offset = 4;
    g_data_offset = artMethodSize - 2 * sizeof(void*);
    g_entry_point_offset = artMethodSize - sizeof(void*);

    // Read the JNI trampoline from hashCode's entry_point.
    // hashCode() is native, so entry_point = art_quick_generic_jni_trampoline.
    g_jni_trampoline = *reinterpret_cast<void**>(
        hashCodeArtMethod + g_entry_point_offset);

    LOGI("ArtMethod: size=%zu, access_flags@%u, data@%u, entry@%u, trampoline=%p",
         artMethodSize, g_access_flags_offset, g_data_offset,
         g_entry_point_offset, g_jni_trampoline);

    if (!g_jni_trampoline) {
        LOGE("Trampoline is null — cannot proceed");
        return false;
    }

    return true;
}

// ============================================================
// Modify an ArtMethod to call our native function instead of its bytecode.
//
// For a native method, ART dispatches via:
//   entry_point_from_quick_compiled_code_ → art_quick_generic_jni_trampoline
//   data_ (entry_point_from_jni_)         → our native function
// ============================================================

static void makeMethodReturnNegativeOne(JNIEnv* env, jobject method, void* nativeFunc) {
    auto* artMethod = reinterpret_cast<uint8_t*>(env->FromReflectedMethod(method));
    if (!artMethod) {
        LOGE("FromReflectedMethod returned null");
        return;
    }

    // Make ArtMethod memory writable
    long pageSize = sysconf(_SC_PAGE_SIZE);
    uintptr_t pageStart = reinterpret_cast<uintptr_t>(artMethod) & ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(pageStart), pageSize * 2,
                 PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect failed for ArtMethod at %p", artMethod);
        return;
    }

    // 1. Set access flags: add kAccNative, clear kAccPreCompiled
    auto* flags = reinterpret_cast<uint32_t*>(artMethod + g_access_flags_offset);
    uint32_t oldFlags = *flags;
    uint32_t newFlags = oldFlags;
    newFlags |= kAccNative;
    if (kAccCompileDontBother) newFlags |= kAccCompileDontBother;
    if (kAccPreCompiled)       newFlags &= ~kAccPreCompiled;
    *flags = newFlags;

    // 2. Set data_ to our native function
    *reinterpret_cast<void**>(artMethod + g_data_offset) = nativeFunc;

    // 3. Set entry_point to the JNI bridge trampoline
    *reinterpret_cast<void**>(artMethod + g_entry_point_offset) = g_jni_trampoline;

    LOGI("Method hooked: old flags=0x%x → new flags=0x%x, native=%p",
         oldFlags, newFlags, nativeFunc);
}

// ============================================================
// Find and hook emoji methods in EmotcationConstants by signature.
//
// Matches: return type int, params all int, param count 1 or 2.
// ============================================================

static void hookEmojiMethods(JNIEnv* env, jclass targetClass) {
    jclass classClass = env->FindClass("java/lang/Class");
    jclass methodClass = env->FindClass("java/lang/reflect/Method");
    jclass integerClass = env->FindClass("java/lang/Integer");

    if (!classClass || !methodClass || !integerClass || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("Cannot find required reflection classes");
        return;
    }

    jfieldID typeId = env->GetStaticFieldID(integerClass, "TYPE", "Ljava/lang/Class;");
    if (!typeId || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("Cannot get Integer.TYPE");
        return;
    }
    jobject intTypeObj = env->GetStaticObjectField(integerClass, typeId);
    jclass intClass = reinterpret_cast<jclass>(env->NewGlobalRef(intTypeObj));
    env->DeleteLocalRef(intTypeObj);

    jmethodID getDeclaredMethods = env->GetMethodID(classClass,
        "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jmethodID getReturnType = env->GetMethodID(methodClass,
        "getReturnType", "()Ljava/lang/Class;");
    jmethodID getParameterTypes = env->GetMethodID(methodClass,
        "getParameterTypes", "()[Ljava/lang/Class;");

    if (!getDeclaredMethods || !getReturnType || !getParameterTypes || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("Cannot get reflection method IDs");
        env->DeleteGlobalRef(intClass);
        return;
    }

    jobjectArray methods = reinterpret_cast<jobjectArray>(
        env->CallObjectMethod(targetClass, getDeclaredMethods));
    if (!methods || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("getDeclaredMethods failed");
        env->DeleteGlobalRef(intClass);
        return;
    }

    int count = env->GetArrayLength(methods);
    int hooked = 0;

    for (int i = 0; i < count && hooked < 2; i++) {
        jobject method = env->GetObjectArrayElement(methods, i);
        if (!method) continue;

        // Return type must be int
        jobject retType = env->CallObjectMethod(method, getReturnType);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(method); continue; }
        bool retOk = env->IsSameObject(retType, intClass);
        env->DeleteLocalRef(retType);
        if (!retOk) { env->DeleteLocalRef(method); continue; }

        // Parameter types must all be int, count 1 or 2
        jobjectArray params = reinterpret_cast<jobjectArray>(
            env->CallObjectMethod(method, getParameterTypes));
        if (env->ExceptionCheck() || !params) {
            env->ExceptionClear();
            env->DeleteLocalRef(method);
            continue;
        }

        int pc = env->GetArrayLength(params);
        bool match = (pc == 1 || pc == 2);
        if (match) {
            for (int j = 0; j < pc && match; j++) {
                jobject pt = env->GetObjectArrayElement(params, j);
                if (!env->IsSameObject(pt, intClass)) match = false;
                env->DeleteLocalRef(pt);
            }
        }
        env->DeleteLocalRef(params);

        if (!match) { env->DeleteLocalRef(method); continue; }

        // Signature matches — modify the ArtMethod
        void* nativeFunc = (pc == 1)
            ? reinterpret_cast<void*>(singleEmojiCallback)
            : reinterpret_cast<void*>(doubleEmojiCallback);

        makeMethodReturnNegativeOne(env, method, nativeFunc);
        hooked++;
        env->DeleteLocalRef(method);
    }

    env->DeleteLocalRef(methods);
    env->DeleteGlobalRef(intClass);

    LOGI("Hooked %d emoji method(s) out of %d declared methods", hooked, count);
}

// ============================================================
// Polling thread: wait for QQ to load EmotcationConstants, then hook.
// ============================================================

static void pollAndHook(JNIEnv* env) {
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (!activityThread || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("ActivityThread not found");
        return;
    }

    jmethodID currentApp = env->GetStaticMethodID(activityThread,
        "currentApplication", "()Landroid/app/Application;");
    if (!currentApp || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("currentApplication method not found");
        env->DeleteLocalRef(activityThread);
        return;
    }

    jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = env->GetMethodID(classLoaderClass,
        "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getClassLoader = env->GetMethodID(contextClass,
        "getClassLoader", "()Ljava/lang/ClassLoader;");

    jstring className = env->NewStringUTF("com.tencent.mobileqq.text.EmotcationConstants");

    // Poll up to 60 seconds (120 attempts × 500ms)
    for (int attempt = 0; attempt < 120; attempt++) {
        env->ExceptionClear();

        jobject app = env->CallStaticObjectMethod(activityThread, currentApp);
        if (env->ExceptionCheck() || !app) {
            env->ExceptionClear();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        jobject classLoader = env->CallObjectMethod(app, getClassLoader);
        env->DeleteLocalRef(app);
        if (env->ExceptionCheck() || !classLoader) {
            env->ExceptionClear();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        jclass targetClass = reinterpret_cast<jclass>(
            env->CallObjectMethod(classLoader, loadClass, className));
        env->DeleteLocalRef(classLoader);

        if (targetClass && !env->ExceptionCheck()) {
            LOGI("EmotcationConstants loaded on attempt %d", attempt);
            hookEmojiMethods(env, targetClass);
            env->DeleteLocalRef(targetClass);
            env->DeleteLocalRef(activityThread);
            env->DeleteLocalRef(classLoaderClass);
            env->DeleteLocalRef(contextClass);
            env->DeleteLocalRef(className);
            return;
        }

        env->ExceptionClear();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOGE("EmotcationConstants not loaded after 60s");
    env->DeleteLocalRef(activityThread);
    env->DeleteLocalRef(classLoaderClass);
    env->DeleteLocalRef(contextClass);
    env->DeleteLocalRef(className);
}

// ============================================================
// Zygisk module entry point
// ============================================================

class QQEmojiModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        env->GetJavaVM(&g_vm);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        JNIEnv* env;
        g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);

        const char* name = env->GetStringUTFChars(args->nice_name, nullptr);
        is_qq = (name && strcmp(name, "com.tencent.mobileqq") == 0);
        env->ReleaseStringUTFChars(args->nice_name, name);

        if (!is_qq) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!is_qq) return;

        JNIEnv* env;
        g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);

        char sdkStr[8] = {0};
        __system_property_get("ro.build.version.sdk", sdkStr);
        int apiLevel = atoi(sdkStr);
        if (apiLevel < 26) {
            LOGE("Android API %d < 26, not supported", apiLevel);
            return;
        }

        initAccessFlags(apiLevel);

        // Determine ArtMethod offsets AND read JNI trampoline from hashCode
        if (!determineOffsets(env)) {
            LOGE("Failed to determine offsets/trampoline");
            return;
        }

        LOGI("Starting polling thread");

        std::thread([]() {
            JNIEnv* env;
            JavaVMAttachArgs attachArgs = {JNI_VERSION_1_6, "QQEmojiHook", nullptr};
            if (g_vm->AttachCurrentThread(&env, &attachArgs) != JNI_OK) {
                LOGE("AttachCurrentThread failed");
                return;
            }
            pollAndHook(env);
            g_vm->DetachCurrentThread();
        }).detach();
    }

private:
    zygisk::Api* api = nullptr;
    bool is_qq = false;
};

REGISTER_ZYGISK_MODULE(QQEmojiModule)
