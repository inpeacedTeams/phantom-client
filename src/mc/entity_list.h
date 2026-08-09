#pragma once
#include <jni.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>

#include "minecraft.h"
#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"

// =================================================================
// EntityList — read world.playerEntities through JNI
// =================================================================
// REFERENCES
// The jobject in EntityInfo is a LOCAL ref, valid only inside the
// local frame ModuleManager pushes for the current tick. Never keep
// one across ticks and never touch it from the render thread. For
// rendering, convert to EntitySnapshot, which is plain data.
//
// CACHING
// Five modules ask for the player list every tick (ESP, AimAssist,
// KillAura, HitSelect, AutoBlockhit). Each scan used to allocate
// fresh local refs for every entity, so a busy lobby produced
// hundreds of refs per tick and pushed the local frame toward
// overflow for no reason. The scan now runs once per tick and the
// rest read from the cache.
// =================================================================

struct EntityInfo {
    jobject ref = nullptr;
    double posX = 0, posY = 0, posZ = 0;
    double prevPosX = 0, prevPosY = 0, prevPosZ = 0;
    float  yaw = 0, pitch = 0;
    float  health = 20.f, maxHealth = 20.f;
    int    hurtTime = 0;
    bool   onGround = false;
    double distanceToPlayer = 0;
    std::string name;
};

// JNI-free copy for the render thread
struct EntitySnapshot {
    double posX = 0, posY = 0, posZ = 0;
    double prevPosX = 0, prevPosY = 0, prevPosZ = 0;
    float  health = 20.f, maxHealth = 20.f;
    int    hurtTime = 0;
    double distanceToPlayer = 0;
    std::string name;
};

class EntityList {
private:
    inline static jfieldID  fPlayerEntities = nullptr;
    inline static jmethodID mListSize = nullptr;
    inline static jmethodID mListGet  = nullptr;
    inline static jmethodID mGetName  = nullptr;
    inline static jmethodID mGetHealth = nullptr;
    inline static jmethodID mGetMaxHealth = nullptr;

    inline static jfieldID fHurtTime = nullptr;
    inline static jfieldID fIsDead   = nullptr;
    inline static jfieldID fOnGround = nullptr;
    inline static jfieldID fPosX = nullptr, fPosY = nullptr, fPosZ = nullptr;
    inline static jfieldID fPrevPosX = nullptr, fPrevPosY = nullptr, fPrevPosZ = nullptr;
    inline static jfieldID fYaw = nullptr, fPitch = nullptr;

    inline static jclass listClass = nullptr;
    inline static bool s_ready = false;
    inline static int  s_retryCooldown = 0;

    // Per-tick cache
    inline static std::vector<EntityInfo> s_cache;
    inline static unsigned long long s_cacheTick = 0;
    inline static unsigned long long s_tick = 0;
    inline static bool s_cacheValid = false;

    static void Scan(JNIEnv* env) {
        s_cache.clear();
        s_cacheValid = true;
        s_cacheTick  = s_tick;

        if (!s_ready) return;

        jobject player = Minecraft::GetPlayer(env);
        jobject world  = Minecraft::GetWorld(env);
        if (!player || !world) return;

        jobject list = env->GetObjectField(world, fPlayerEntities);
        if (!list) return;

        jint size = env->CallIntMethod(list, mListSize);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (size <= 0 || size > 512) return;   // sanity

        double pX = env->GetDoubleField(player, fPosX);
        double pY = env->GetDoubleField(player, fPosY);
        double pZ = env->GetDoubleField(player, fPosZ);

        s_cache.reserve((size_t)size);

        for (jint i = 0; i < size; i++) {
            jobject ent = env->CallObjectMethod(list, mListGet, i);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!ent) continue;

            if (env->IsSameObject(ent, player)) { env->DeleteLocalRef(ent); continue; }

            if (fIsDead && env->GetBooleanField(ent, fIsDead)) {
                env->DeleteLocalRef(ent);
                continue;
            }

            EntityInfo info;
            info.posX = env->GetDoubleField(ent, fPosX);
            info.posY = env->GetDoubleField(ent, fPosY);
            info.posZ = env->GetDoubleField(ent, fPosZ);

            double dx = info.posX - pX, dy = info.posY - pY, dz = info.posZ - pZ;
            info.distanceToPlayer = std::sqrt(dx*dx + dy*dy + dz*dz);

            // Nothing in this client looks past 128 blocks
            if (info.distanceToPlayer > 128.0) {
                env->DeleteLocalRef(ent);
                continue;
            }

            float hp = 20.f;
            if (mGetHealth) {
                hp = env->CallFloatMethod(ent, mGetHealth);
                if (env->ExceptionCheck()) { env->ExceptionClear(); hp = 20.f; }
            }
            if (hp <= 0.f) { env->DeleteLocalRef(ent); continue; }
            info.health = hp;

            info.ref = ent;   // kept: freed when the tick's frame pops

            info.prevPosX = fPrevPosX ? env->GetDoubleField(ent, fPrevPosX) : info.posX;
            info.prevPosY = fPrevPosY ? env->GetDoubleField(ent, fPrevPosY) : info.posY;
            info.prevPosZ = fPrevPosZ ? env->GetDoubleField(ent, fPrevPosZ) : info.posZ;
            info.yaw      = fYaw   ? env->GetFloatField(ent, fYaw)   : 0.f;
            info.pitch    = fPitch ? env->GetFloatField(ent, fPitch) : 0.f;
            info.hurtTime = fHurtTime ? env->GetIntField(ent, fHurtTime) : 0;
            info.onGround = fOnGround ? env->GetBooleanField(ent, fOnGround) : false;

            if (mGetMaxHealth) {
                info.maxHealth = env->CallFloatMethod(ent, mGetMaxHealth);
                if (env->ExceptionCheck()) { env->ExceptionClear(); info.maxHealth = 20.f; }
            }
            if (info.maxHealth <= 0.f) info.maxHealth = 20.f;

            if (mGetName) {
                jstring js = (jstring)env->CallObjectMethod(ent, mGetName);
                if (env->ExceptionCheck()) env->ExceptionClear();
                else if (js) {
                    const char* c = env->GetStringUTFChars(js, nullptr);
                    if (c) { info.name = c; env->ReleaseStringUTFChars(js, c); }
                    env->DeleteLocalRef(js);
                }
            }

            s_cache.push_back(std::move(info));
        }

        env->DeleteLocalRef(list);
        env->DeleteLocalRef(world);
        env->DeleteLocalRef(player);
    }

public:
    static bool IsReady() { return s_ready; }

    // ModuleManager calls this at the top of every tick, right after
    // pushing the local frame. The cached refs belong to that frame.
    static void BeginTick() {
        s_tick++;
        s_cacheValid = false;
        s_cache.clear();
    }

    static bool Init(JNIEnv* env) {
        if (s_ready) return true;

        // playerEntities only exists once a world is loaded, so this
        // gets retried from OnTick. Throttle it so we are not walking
        // the class table every tick while sitting in a menu.
        if (s_retryCooldown > 0) { s_retryCooldown--; return false; }
        s_retryCooldown = 20;

        if (!listClass) {
            jclass lc = env->FindClass("java/util/List");
            if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
            listClass = (jclass)env->NewGlobalRef(lc);
            env->DeleteLocalRef(lc);

            mListSize = env->GetMethodID(listClass, "size", "()I");
            if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
            mListGet = env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        }

        // Resolve against the live world object's own class; the
        // entry in the class table may be a different World subclass.
        jobject world = Minecraft::GetWorld(env);
        if (!world) return false;

        jclass worldCls = env->GetObjectClass(world);
        fPlayerEntities = JvmtiUtil::FindField(env, worldCls,
            { "field_73010_i", "playerEntities" });
        env->DeleteLocalRef(worldCls);
        env->DeleteLocalRef(world);

        if (!fPlayerEntities) return false;

        if (ClassResolver::entity) {
            auto e = ClassResolver::entity;
            fPosX     = JvmtiUtil::FindField(env, e, { "field_70165_t", "posX" });
            fPosY     = JvmtiUtil::FindField(env, e, { "field_70163_u", "posY" });
            fPosZ     = JvmtiUtil::FindField(env, e, { "field_70161_v", "posZ" });
            fPrevPosX = JvmtiUtil::FindField(env, e, { "field_70169_q", "prevPosX" });
            fPrevPosY = JvmtiUtil::FindField(env, e, { "field_70167_r", "prevPosY" });
            fPrevPosZ = JvmtiUtil::FindField(env, e, { "field_70166_s", "prevPosZ" });
            fYaw      = JvmtiUtil::FindField(env, e, { "field_70177_z", "rotationYaw" });
            fPitch    = JvmtiUtil::FindField(env, e, { "field_70125_A", "rotationPitch" });
            fOnGround = JvmtiUtil::FindField(env, e, { "field_70122_E", "onGround" });
            fIsDead   = JvmtiUtil::FindField(env, e, { "field_70128_L", "isDead" });
            mGetName  = JvmtiUtil::FindMethod(env, e, { "func_70005_c_", "getName" }, 0);
        }

        // Health lives in the DataWatcher in 1.8, so it has to come
        // from getHealth() rather than a field read.
        if (ClassResolver::entityLivingBase) {
            auto lb = ClassResolver::entityLivingBase;
            fHurtTime     = JvmtiUtil::FindField(env, lb, { "field_70737_aN", "hurtTime" });
            mGetHealth    = JvmtiUtil::FindMethod(env, lb, { "func_110143_aJ", "getHealth" }, 0);
            mGetMaxHealth = JvmtiUtil::FindMethod(env, lb, { "func_110138_aP", "getMaxHealth" }, 0);
        }

        s_ready = (fPlayerEntities && mListSize && mListGet && fPosX);
        printf("[EntityList] ready=%d list=%p health=%p name=%p\n",
            (int)s_ready, (void*)fPlayerEntities, (void*)mGetHealth, (void*)mGetName);
        return s_ready;
    }

    static void Shutdown(JNIEnv* env) {
        s_cache.clear();
        s_cacheValid = false;
        if (listClass) { env->DeleteGlobalRef(listClass); listClass = nullptr; }
        s_ready = false;
    }

    // Players within maxRange, excluding the local player. Scans at
    // most once per tick; later callers reuse the cache.
    static std::vector<EntityInfo> GetPlayers(JNIEnv* env, float maxRange = 64.0f) {
        if (!s_ready) return {};

        if (!s_cacheValid || s_cacheTick != s_tick) Scan(env);

        std::vector<EntityInfo> out;
        out.reserve(s_cache.size());
        for (const auto& e : s_cache)
            if (e.distanceToPlayer <= maxRange) out.push_back(e);
        return out;
    }

    static std::vector<EntitySnapshot> ToSnapshots(const std::vector<EntityInfo>& in) {
        std::vector<EntitySnapshot> out;
        out.reserve(in.size());
        for (const auto& e : in) {
            EntitySnapshot s;
            s.posX = e.posX; s.posY = e.posY; s.posZ = e.posZ;
            s.prevPosX = e.prevPosX; s.prevPosY = e.prevPosY; s.prevPosZ = e.prevPosZ;
            s.health = e.health; s.maxHealth = e.maxHealth;
            s.hurtTime = e.hurtTime;
            s.distanceToPlayer = e.distanceToPlayer;
            s.name = e.name;
            out.push_back(std::move(s));
        }
        return out;
    }

    // ---- Target selection ----
    static EntityInfo* FindClosest(std::vector<EntityInfo>& list, float maxRange) {
        EntityInfo* best = nullptr;
        double bestDist = maxRange;
        for (auto& e : list)
            if (e.distanceToPlayer < bestDist) { bestDist = e.distanceToPlayer; best = &e; }
        return best;
    }

    static EntityInfo* FindLowestHP(std::vector<EntityInfo>& list, float maxRange) {
        EntityInfo* best = nullptr;
        float bestHP = 1e9f;
        for (auto& e : list) {
            if (e.distanceToPlayer > maxRange) continue;
            if (e.health < bestHP) { bestHP = e.health; best = &e; }
        }
        return best;
    }

    static EntityInfo* FindClosestToCrosshair(JNIEnv* env, std::vector<EntityInfo>& list,
                                              float fov, float maxRange)
    {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return nullptr;

        float pYaw   = Minecraft::GetYaw(env, player);
        float pPitch = Minecraft::GetPitch(env, player);

        EntityInfo* best = nullptr;
        float bestAngle = fov * 0.5f;

        for (auto& e : list) {
            if (e.distanceToPlayer > maxRange) continue;

            auto rot = Minecraft::GetRotationsToPos(env, player,
                e.posX, e.posY + 1.0, e.posZ);

            float dy = rot.yaw - pYaw;
            while (dy > 180.f)  dy -= 360.f;
            while (dy < -180.f) dy += 360.f;
            float dp = rot.pitch - pPitch;

            float angle = std::sqrt(dy*dy + dp*dp);
            if (angle < bestAngle) { bestAngle = angle; best = &e; }
        }

        env->DeleteLocalRef(player);
        return best;
    }
};
