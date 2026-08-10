#pragma once
#include <jni.h>
#include <cmath>
#include <cstdio>

#include "minecraft.h"
#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"

// =================================================================
// World
// =================================================================
// Asks the world what is at a block position.
//
// WHY THIS EXISTS
// Bridge Assist used to decide it was at an edge by looking at the
// fractional part of the player's coordinates: near a block
// boundary meant sneak. That is wrong in the common case. Walking
// across a flat floor crosses a boundary every single block, so it
// sneaked constantly on solid ground and did nothing useful.
//
// The real question is whether there is air under the spot you are
// about to step onto, and that needs the world.
//
// THE CALL CHAIN
//   world.getBlockState(BlockPos)  ->  IBlockState
//   state.getBlock()               ->  Block
//   block.getMaterial()            ->  Material
//   material.isSolid()             ->  boolean
//
// Every link is obfuscated, so each is resolved by SRG name with an
// MCP fallback through JvmtiUtil. If any link fails the whole thing
// reports unusable and callers fall back to their old behaviour
// rather than guessing.
// =================================================================

class World {
private:
    inline static jclass    s_blockPosClass = nullptr;   // global ref
    inline static jmethodID s_blockPosCtor  = nullptr;   // (III)V
    inline static jmethodID s_getBlockState = nullptr;
    inline static jmethodID s_getBlock      = nullptr;
    inline static jmethodID s_getMaterial   = nullptr;
    inline static jmethodID s_isSolid       = nullptr;
    inline static jmethodID s_isLiquid      = nullptr;

    inline static bool s_resolved = false;
    inline static bool s_usable   = false;
    inline static int  s_retry    = 0;

    // Find BlockPos by walking the loaded classes for one that has
    // an (int,int,int) constructor and the getX/getY/getZ trio.
    static jclass FindBlockPos(JNIEnv* env, jobject world) {
        // Cheapest route: ask an existing method for its parameter
        // type. getBlockState takes exactly one BlockPos.
        jclass wc = env->GetObjectClass(world);

        std::string sig;
        jmethodID m = JvmtiUtil::FindMethod(env, wc,
            { "func_180495_p", "getBlockState" }, 1, &sig);
        env->DeleteLocalRef(wc);
        if (!m || sig.empty()) return nullptr;

        // sig looks like "(Lnet/minecraft/util/BlockPos;)Lsomething;"
        size_t open = sig.find('L');
        size_t semi = sig.find(';');
        if (open == std::string::npos || semi == std::string::npos || semi <= open)
            return nullptr;

        std::string cls = sig.substr(open + 1, semi - open - 1);
        jclass bp = env->FindClass(cls.c_str());
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        return bp;
    }

public:
    static bool IsUsable() { return s_usable; }

    static bool Init(JNIEnv* env) {
        if (s_resolved) return s_usable;

        // Only exists once a world is loaded, so throttle the retry
        // rather than walking the class table every tick in a menu.
        if (s_retry > 0) { s_retry--; return false; }
        s_retry = 20;

        jobject world = Minecraft::GetWorld(env);
        if (!world) return false;

        jclass wc = env->GetObjectClass(world);
        s_getBlockState = JvmtiUtil::FindMethod(env, wc,
            { "func_180495_p", "getBlockState" }, 1);
        env->DeleteLocalRef(wc);

        if (!s_getBlockState) { env->DeleteLocalRef(world); return false; }

        jclass bp = FindBlockPos(env, world);
        if (bp) {
            s_blockPosClass = (jclass)env->NewGlobalRef(bp);
            s_blockPosCtor = env->GetMethodID(bp, "<init>", "(III)V");
            if (env->ExceptionCheck()) { env->ExceptionClear(); s_blockPosCtor = nullptr; }
            env->DeleteLocalRef(bp);
        }

        // Probe the rest of the chain with a real lookup at the
        // player's feet, because the return types are only knowable
        // from an actual instance.
        if (s_blockPosClass && s_blockPosCtor) {
            jobject player = Minecraft::GetPlayer(env);
            if (player) {
                int px = (int)std::floor(Minecraft::GetPosX(env, player));
                int py = (int)std::floor(Minecraft::GetPosY(env, player)) - 1;
                int pz = (int)std::floor(Minecraft::GetPosZ(env, player));

                jobject pos = env->NewObject(s_blockPosClass, s_blockPosCtor,
                                             (jint)px, (jint)py, (jint)pz);
                if (!env->ExceptionCheck() && pos) {
                    jobject state = env->CallObjectMethod(world, s_getBlockState, pos);
                    if (!env->ExceptionCheck() && state) {
                        jclass sc = env->GetObjectClass(state);
                        s_getBlock = JvmtiUtil::FindMethod(env, sc,
                            { "func_177230_c", "getBlock" }, 0);
                        env->DeleteLocalRef(sc);

                        if (s_getBlock) {
                            jobject block = env->CallObjectMethod(state, s_getBlock);
                            if (!env->ExceptionCheck() && block) {
                                jclass bc = env->GetObjectClass(block);
                                s_getMaterial = JvmtiUtil::FindMethod(env, bc,
                                    { "func_149688_o", "getMaterial" }, 0);
                                env->DeleteLocalRef(bc);

                                if (s_getMaterial) {
                                    jobject mat = env->CallObjectMethod(block, s_getMaterial);
                                    if (!env->ExceptionCheck() && mat) {
                                        jclass mc = env->GetObjectClass(mat);
                                        s_isSolid = JvmtiUtil::FindMethod(env, mc,
                                            { "func_76220_a", "isSolid" }, 0);
                                        s_isLiquid = JvmtiUtil::FindMethod(env, mc,
                                            { "func_76224_d", "isLiquid" }, 0);
                                        env->DeleteLocalRef(mc);
                                        env->DeleteLocalRef(mat);
                                    } else if (env->ExceptionCheck()) env->ExceptionClear();
                                }
                                env->DeleteLocalRef(block);
                            } else if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        env->DeleteLocalRef(state);
                    } else if (env->ExceptionCheck()) env->ExceptionClear();
                    env->DeleteLocalRef(pos);
                } else if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }

        env->DeleteLocalRef(world);

        s_usable = (s_getBlockState && s_blockPosClass && s_blockPosCtor
                 && s_getBlock && s_getMaterial && s_isSolid);
        s_resolved = true;

        printf("[World] state=%p pos=%p block=%p mat=%p solid=%p usable=%d\n",
            (void*)s_getBlockState, (void*)s_blockPosClass, (void*)s_getBlock,
            (void*)s_getMaterial, (void*)s_isSolid, (int)s_usable);

        return s_usable;
    }

    static void Shutdown(JNIEnv* env) {
        if (s_blockPosClass) {
            env->DeleteGlobalRef(s_blockPosClass);
            s_blockPosClass = nullptr;
        }
        s_resolved = false;
        s_usable = false;
    }

    // Is the block at these coordinates something you can stand on?
    // Returns true when unusable, so callers treat unknown ground as
    // safe rather than sneaking everywhere.
    static bool IsSolid(JNIEnv* env, int x, int y, int z) {
        if (!s_usable) return true;

        jobject world = Minecraft::GetWorld(env);
        if (!world) return true;

        bool solid = true;

        jobject pos = env->NewObject(s_blockPosClass, s_blockPosCtor,
                                     (jint)x, (jint)y, (jint)z);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }

        jobject state = env->CallObjectMethod(world, s_getBlockState, pos);
        if (env->ExceptionCheck()) { env->ExceptionClear(); state = nullptr; }

        if (state) {
            jobject block = env->CallObjectMethod(state, s_getBlock);
            if (env->ExceptionCheck()) { env->ExceptionClear(); block = nullptr; }

            if (block) {
                jobject mat = env->CallObjectMethod(block, s_getMaterial);
                if (env->ExceptionCheck()) { env->ExceptionClear(); mat = nullptr; }

                if (mat) {
                    jboolean v = env->CallBooleanMethod(mat, s_isSolid);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    else solid = (v != 0);
                    env->DeleteLocalRef(mat);
                }
                env->DeleteLocalRef(block);
            }
            env->DeleteLocalRef(state);
        }

        env->DeleteLocalRef(pos);
        env->DeleteLocalRef(world);
        return solid;
    }

    // Convenience: is there something to stand on just below this
    // world position?
    static bool GroundBelow(JNIEnv* env, double x, double y, double z) {
        return IsSolid(env,
            (int)std::floor(x),
            (int)std::floor(y) - 1,
            (int)std::floor(z));
    }
};
