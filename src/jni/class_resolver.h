#pragma once
#include <jni.h>
#include <jvmti.h>
#include <string>
#include <cstdio>

#include "jvmti_util.h"

// =================================================================
// ClassResolver
// =================================================================
// Lunar uses a custom classloader, so env->FindClass() cannot see
// Minecraft classes. We enumerate every loaded class through JVMTI
// and identify them by the fields they declare.
//
// Field matching is done by NAME ONLY (via JvmtiUtil), because the
// signatures are obfuscated and unknowable ahead of time.
// =================================================================

class ClassResolver {
public:
    inline static jclass mcClass          = nullptr; // Minecraft
    inline static jclass entityPlayerSP   = nullptr; // EntityPlayerSP
    inline static jclass entityPlayer     = nullptr; // EntityPlayer
    inline static jclass entityLivingBase = nullptr; // EntityLivingBase
    inline static jclass entity           = nullptr; // Entity
    inline static jclass world            = nullptr; // World / WorldClient
    inline static jclass playerController = nullptr; // PlayerControllerMP
    inline static jclass gameSettings     = nullptr; // GameSettings
    inline static jclass keyBinding       = nullptr; // KeyBinding
    inline static jclass timerClass       = nullptr; // Timer

    inline static bool resolved = false;

    static bool ResolveAll(JNIEnv* env) {
        if (resolved) return mcClass != nullptr;

        jvmtiEnv* jvmti = JvmtiUtil::Get(env);
        if (!jvmti) {
            printf("[Resolver] JVMTI unavailable\n");
            return false;
        }

        jint classCount = 0;
        jclass* classes = nullptr;
        if (jvmti->GetLoadedClasses(&classCount, &classes) != JVMTI_ERROR_NONE || !classes) {
            printf("[Resolver] GetLoadedClasses failed\n");
            return false;
        }
        printf("[Resolver] Scanning %d loaded classes...\n", classCount);

        for (jint i = 0; i < classCount; i++) {
            jclass c = classes[i];

            // ---- Minecraft: declares both thePlayer and theWorld ----
            if (!mcClass
                && JvmtiUtil::HasField(env, c, "thePlayer")
                && JvmtiUtil::HasField(env, c, "theWorld")) {
                mcClass = (jclass)env->NewGlobalRef(c);
                printf("[Resolver] Minecraft -> %s\n",
                    JvmtiUtil::GetClassSignature(env, c).c_str());
            }
            if (!mcClass
                && JvmtiUtil::HasField(env, c, "field_71439_g")
                && JvmtiUtil::HasField(env, c, "field_71441_e")) {
                mcClass = (jclass)env->NewGlobalRef(c);
                printf("[Resolver] Minecraft (SRG) -> %s\n",
                    JvmtiUtil::GetClassSignature(env, c).c_str());
            }

            // ---- EntityPlayerSP: declares sendQueue ----
            if (!entityPlayerSP
                && (JvmtiUtil::HasField(env, c, "sendQueue")
                 || JvmtiUtil::HasField(env, c, "field_71174_a"))) {
                entityPlayerSP = (jclass)env->NewGlobalRef(c);
                printf("[Resolver] EntityPlayerSP -> %s\n",
                    JvmtiUtil::GetClassSignature(env, c).c_str());
            }

            // ---- GameSettings: declares gammaSetting ----
            if (!gameSettings
                && (JvmtiUtil::HasField(env, c, "gammaSetting")
                 || JvmtiUtil::HasField(env, c, "field_74333_Y"))) {
                gameSettings = (jclass)env->NewGlobalRef(c);
                printf("[Resolver] GameSettings -> %s\n",
                    JvmtiUtil::GetClassSignature(env, c).c_str());
            }

            // ---- KeyBinding: declares pressed + keyCode ----
            if (!keyBinding
                && (JvmtiUtil::HasField(env, c, "pressed")
                 || JvmtiUtil::HasField(env, c, "field_74513_e"))
                && (JvmtiUtil::HasField(env, c, "keyCode")
                 || JvmtiUtil::HasField(env, c, "field_74512_d"))) {
                keyBinding = (jclass)env->NewGlobalRef(c);
                printf("[Resolver] KeyBinding -> %s\n",
                    JvmtiUtil::GetClassSignature(env, c).c_str());
            }

            // ---- Timer: declares timerSpeed + renderPartialTicks ----
            if (!timerClass
                && (JvmtiUtil::HasField(env, c, "timerSpeed")
                 || JvmtiUtil::HasField(env, c, "field_74278_d"))) {
                timerClass = (jclass)env->NewGlobalRef(c);
            }

            // ---- PlayerControllerMP: declares curBlockDamageMP ----
            if (!playerController
                && (JvmtiUtil::HasField(env, c, "curBlockDamageMP")
                 || JvmtiUtil::HasField(env, c, "field_78770_f"))) {
                playerController = (jclass)env->NewGlobalRef(c);
                printf("[Resolver] PlayerControllerMP -> %s\n",
                    JvmtiUtil::GetClassSignature(env, c).c_str());
            }

            // ---- World: declares playerEntities + loadedEntityList ----
            if (!world
                && (JvmtiUtil::HasField(env, c, "playerEntities")
                 || JvmtiUtil::HasField(env, c, "field_73010_i"))) {
                world = (jclass)env->NewGlobalRef(c);
                printf("[Resolver] World -> %s\n",
                    JvmtiUtil::GetClassSignature(env, c).c_str());
            }
        }

        jvmti->Deallocate((unsigned char*)classes);

        // ---- Derive the entity hierarchy from EntityPlayerSP ----
        // EntityPlayerSP -> AbstractClientPlayer -> EntityPlayer
        //   -> EntityLivingBase -> Entity
        // Depth varies between mappings, so identify each level by
        // the fields it declares rather than by counting supers.
        if (entityPlayerSP) {
            jclass cur = env->GetSuperclass(entityPlayerSP);
            while (cur) {
                if (!entityPlayer
                    && (JvmtiUtil::HasField(env, cur, "inventory")
                     || JvmtiUtil::HasField(env, cur, "field_71071_by"))) {
                    entityPlayer = (jclass)env->NewGlobalRef(cur);
                    printf("[Resolver] EntityPlayer resolved\n");
                }
                if (!entityLivingBase
                    && (JvmtiUtil::HasField(env, cur, "hurtTime")
                     || JvmtiUtil::HasField(env, cur, "field_70737_aN"))) {
                    entityLivingBase = (jclass)env->NewGlobalRef(cur);
                    printf("[Resolver] EntityLivingBase resolved\n");
                }
                if (!entity
                    && (JvmtiUtil::HasField(env, cur, "posX")
                     || JvmtiUtil::HasField(env, cur, "field_70165_t"))) {
                    entity = (jclass)env->NewGlobalRef(cur);
                    printf("[Resolver] Entity resolved\n");
                }

                jclass super = env->GetSuperclass(cur);
                env->DeleteLocalRef(cur);
                cur = super;
            }
        }

        resolved = (mcClass != nullptr);

        if (!resolved) {
            printf("[Resolver] FAILED: Minecraft class not found.\n");
            printf("[Resolver] Run DumpAllClasses() and search manually.\n");
        } else {
            printf("[Resolver] mc=%p player=%p living=%p entity=%p world=%p gs=%p kb=%p pc=%p\n",
                (void*)mcClass, (void*)entityPlayerSP, (void*)entityLivingBase,
                (void*)entity, (void*)world, (void*)gameSettings,
                (void*)keyBinding, (void*)playerController);
        }
        return resolved;
    }

    // Some classes (World, PlayerController) only load once you join
    // a server. Call this after the world exists to fill the gaps.
    static void ResolveLate(JNIEnv* env) {
        if (world && playerController && entity) return;
        resolved = false;
        // Clear only the ones still missing so we do not leak globals
        ResolveAll(env);
    }

    static void DumpAllClasses(JNIEnv* env) {
        jvmtiEnv* jvmti = JvmtiUtil::Get(env);
        if (!jvmti) return;

        jint count = 0;
        jclass* classes = nullptr;
        if (jvmti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE) return;

        for (jint i = 0; i < count; i++) {
            char* sig = nullptr;
            if (jvmti->GetClassSignature(classes[i], &sig, nullptr) == JVMTI_ERROR_NONE && sig) {
                printf("%s\n", sig);
                jvmti->Deallocate((unsigned char*)sig);
            }
        }
        jvmti->Deallocate((unsigned char*)classes);
    }
};
