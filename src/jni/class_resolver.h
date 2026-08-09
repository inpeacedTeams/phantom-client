#pragma once
#include <jni.h>
#include <jvmti.h>
#include <string>
#include <cstdio>

// Lunar uses a custom classloader, so env->FindClass() won't work
// for Minecraft classes. We enumerate all loaded classes via JVMTI
// and identify them by their unique fields/methods.

class ClassResolver {
public:
    inline static jclass mcClass           = nullptr; // Minecraft
    inline static jclass entityPlayerSP    = nullptr; // EntityPlayerSP
    inline static jclass entityPlayer      = nullptr; // EntityPlayer
    inline static jclass entityLivingBase  = nullptr; // EntityLivingBase
    inline static jclass entity            = nullptr; // Entity
    inline static jclass world             = nullptr; // World
    inline static jclass worldClient       = nullptr; // WorldClient
    inline static jclass playerController  = nullptr; // PlayerControllerMP
    inline static jclass renderManager     = nullptr; // RenderManager
    inline static jclass activeRenderInfo  = nullptr; // ActiveRenderInfo
    inline static jclass gameSettings      = nullptr; // GameSettings
    inline static jclass timerClass        = nullptr; // Timer
    inline static jclass axisAlignedBB     = nullptr; // AxisAlignedBB
    inline static jclass networkManager    = nullptr; // NetworkManager
    inline static jclass inventoryPlayer   = nullptr; // InventoryPlayer
    inline static jclass itemStack         = nullptr; // ItemStack

    static bool ResolveAll(JNIEnv* env) {
        JavaVM* vm;
        env->GetJavaVM(&vm);

        jvmtiEnv* jvmti = nullptr;
        if (vm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_2) != JNI_OK || !jvmti) {
            printf("[Resolver] JVMTI not available\n");
            return false;
        }

        jint classCount = 0;
        jclass* classes = nullptr;
        jvmti->GetLoadedClasses(&classCount, &classes);
        printf("[Resolver] Scanning %d loaded classes...\n", classCount);

        // Mapping: we try both MCP (field_XXXXX) and Notch names
        // Lunar 1.8.9 typically uses Notch mappings with some SRG

        for (jint i = 0; i < classCount; i++) {
            char* sig = nullptr;
            jvmti->GetClassSignature(classes[i], &sig, nullptr);
            if (!sig) continue;

            std::string s(sig);

            // Minecraft: has thePlayer/field_71439_g
            if (!mcClass && (
                HasField(env, classes[i], "thePlayer", "Ljava/lang/Object;") ||
                HasField(env, classes[i], "field_71439_g", "Ljava/lang/Object;")
            )) {
                mcClass = (jclass)env->NewGlobalRef(classes[i]);
                printf("[Resolver] Minecraft -> %s\n", sig);
            }

            // EntityPlayerSP: has sendQueue/field_71174_a
            if (!entityPlayerSP && (
                HasField(env, classes[i], "sendQueue", "Ljava/lang/Object;") ||
                HasField(env, classes[i], "field_71174_a", "Ljava/lang/Object;")
            )) {
                entityPlayerSP = (jclass)env->NewGlobalRef(classes[i]);
                printf("[Resolver] EntityPlayerSP -> %s\n", sig);
            }

            // GameSettings: has gammaSetting/field_74333_Y
            if (!gameSettings && (
                HasFieldFloat(env, classes[i], "gammaSetting") ||
                HasFieldFloat(env, classes[i], "field_74333_Y")
            )) {
                gameSettings = (jclass)env->NewGlobalRef(classes[i]);
                printf("[Resolver] GameSettings -> %s\n", sig);
            }

            // AxisAlignedBB: has minX, minY, minZ, maxX, maxY, maxZ
            if (!axisAlignedBB && 
                HasFieldDouble(env, classes[i], "minX") &&
                HasFieldDouble(env, classes[i], "maxX")
            ) {
                axisAlignedBB = (jclass)env->NewGlobalRef(classes[i]);
                printf("[Resolver] AxisAlignedBB -> %s\n", sig);
            }

            jvmti->Deallocate((unsigned char*)sig);
        }

        jvmti->Deallocate((unsigned char*)classes);

        // Resolve parent classes from EntityPlayerSP
        if (entityPlayerSP) {
            jclass superClass = env->GetSuperclass(entityPlayerSP);
            if (superClass) {
                entityPlayer = (jclass)env->NewGlobalRef(superClass);
                printf("[Resolver] EntityPlayer -> (super of EntityPlayerSP)\n");

                jclass super2 = env->GetSuperclass(superClass);
                if (super2) {
                    entityLivingBase = (jclass)env->NewGlobalRef(super2);
                    printf("[Resolver] EntityLivingBase -> (super of EntityPlayer)\n");

                    jclass super3 = env->GetSuperclass(super2);
                    if (super3) {
                        entity = (jclass)env->NewGlobalRef(super3);
                        printf("[Resolver] Entity -> (super of EntityLivingBase)\n");
                    }
                }
            }
        }

        bool success = (mcClass != nullptr);
        if (!success) {
            printf("[Resolver] FAILED: Could not find Minecraft class\n");
            printf("[Resolver] Tip: dump all class names and search manually\n");
        }
        return success;
    }

    // Utility: dump all class names to console (for reverse engineering)
    static void DumpAllClasses(JNIEnv* env) {
        JavaVM* vm;
        env->GetJavaVM(&vm);
        jvmtiEnv* jvmti = nullptr;
        vm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_2);
        if (!jvmti) return;

        jint count = 0;
        jclass* classes = nullptr;
        jvmti->GetLoadedClasses(&count, &classes);

        for (jint i = 0; i < count; i++) {
            char* sig = nullptr;
            jvmti->GetClassSignature(classes[i], &sig, nullptr);
            if (sig) {
                printf("%s\n", sig);
                jvmti->Deallocate((unsigned char*)sig);
            }
        }
        jvmti->Deallocate((unsigned char*)classes);
    }

private:
    static bool HasField(JNIEnv* env, jclass cls, const char* name, const char* sig) {
        jfieldID f = env->GetFieldID(cls, name, sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return f != nullptr;
    }

    static bool HasFieldFloat(JNIEnv* env, jclass cls, const char* name) {
        jfieldID f = env->GetFieldID(cls, name, "F");
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return f != nullptr;
    }

    static bool HasFieldDouble(JNIEnv* env, jclass cls, const char* name) {
        jfieldID f = env->GetFieldID(cls, name, "D");
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return f != nullptr;
    }
};
