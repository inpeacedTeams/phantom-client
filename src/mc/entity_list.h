#pragma once
#include <jni.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>

#include "minecraft.h"
#include "../jni/class_resolver.h"
#include "../mappings/mcp189.h"

// =================================================================
// EntityList — iterate world.playerEntities via JNI
// =================================================================
// world.playerEntities is a java.util.List<EntityPlayer>.
// We access it via JNI reflection:
//   1. Get theWorld object from Minecraft
//   2. Get playerEntities field (List)
//   3. Call List.size() and List.get(i)
//   4. For each entity: extract pos, health, name, etc.
//   5. Skip self (compare with thePlayer)
// =================================================================

struct EntityInfo {
    jobject ref;            // JNI local ref (valid only during current tick)
    double posX, posY, posZ;
    double prevPosX, prevPosY, prevPosZ;
    float yaw, pitch;
    float health;
    float maxHealth;
    int hurtTime;
    bool isDead;
    bool onGround;
    double distanceToPlayer;
    std::string name;
};

class EntityList {
private:
    // Cached JNI IDs
    inline static jfieldID fPlayerEntities = nullptr;   // World.playerEntities
    inline static jmethodID mListSize = nullptr;        // List.size()
    inline static jmethodID mListGet = nullptr;         // List.get(int)
    inline static jmethodID mGetName = nullptr;         // Entity.getName()
    inline static jmethodID mGetMaxHealth = nullptr;    // EntityLivingBase.getMaxHealth()
    inline static jfieldID fHealth = nullptr;            // EntityLivingBase.health
    inline static jfieldID fHurtTime = nullptr;
    inline static jfieldID fIsDead = nullptr;
    inline static jfieldID fPosX = nullptr, fPosY = nullptr, fPosZ = nullptr;
    inline static jfieldID fPrevPosX = nullptr, fPrevPosY = nullptr, fPrevPosZ = nullptr;
    inline static jfieldID fYaw = nullptr, fPitch = nullptr;
    inline static jfieldID fOnGround = nullptr;

    inline static jclass listClass = nullptr;
    inline static bool s_resolved = false;

public:
    static bool Init(JNIEnv* env) {
        if (s_resolved) return true;

        // --- List class ---
        listClass = env->FindClass("java/util/List");
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        listClass = (jclass)env->NewGlobalRef(listClass);

        mListSize = env->GetMethodID(listClass, "size", "()I");
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        mListGet = env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        // --- World.playerEntities ---
        // Try SRG then MCP name
        if (ClassResolver::world) {
            fPlayerEntities = env->GetFieldID(ClassResolver::world,
                MCP::World::playerEntities_srg, "Ljava/util/List;");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                fPlayerEntities = env->GetFieldID(ClassResolver::world,
                    MCP::World::playerEntities_mcp, "Ljava/util/List;");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }

        // If world class not resolved yet, try to find playerEntities
        // on the actual world object's class
        if (!fPlayerEntities) {
            jobject worldObj = Minecraft::GetWorld(env);
            if (worldObj) {
                jclass worldCls = env->GetObjectClass(worldObj);
                // Walk up the hierarchy to find playerEntities
                jclass cls = worldCls;
                for (int depth = 0; depth < 5 && cls && !fPlayerEntities; depth++) {
                    fPlayerEntities = env->GetFieldID(cls,
                        MCP::World::playerEntities_srg, "Ljava/util/List;");
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        fPlayerEntities = env->GetFieldID(cls,
                            MCP::World::playerEntities_mcp, "Ljava/util/List;");
                        if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                            fPlayerEntities = nullptr;
                        }
                    }
                    if (!fPlayerEntities) {
                        cls = env->GetSuperclass(cls);
                    }
                }
            }
        }

        // --- Entity fields ---
        if (ClassResolver::entity) {
            auto e = ClassResolver::entity;
            fPosX = GetFieldSafe(env, e, MCP::Entity::posX_srg, MCP::Entity::posX_mcp, "D");
            fPosY = GetFieldSafe(env, e, MCP::Entity::posY_srg, MCP::Entity::posY_mcp, "D");
            fPosZ = GetFieldSafe(env, e, MCP::Entity::posZ_srg, MCP::Entity::posZ_mcp, "D");

            fPrevPosX = GetFieldSafe(env, e, MCP::Entity::prevPosX_srg, "prevPosX", "D");
            fPrevPosY = GetFieldSafe(env, e, MCP::Entity::prevPosY_srg, "prevPosY", "D");
            fPrevPosZ = GetFieldSafe(env, e, MCP::Entity::prevPosZ_srg, "prevPosZ", "D");

            fYaw = GetFieldSafe(env, e, MCP::Entity::rotationYaw_srg, MCP::Entity::rotationYaw_mcp, "F");
            fPitch = GetFieldSafe(env, e, MCP::Entity::rotationPitch_srg, MCP::Entity::rotationPitch_mcp, "F");
            fOnGround = GetFieldSafe(env, e, MCP::Entity::onGround_srg, MCP::Entity::onGround_mcp, "Z");
            fIsDead = GetFieldSafe(env, e, MCP::Entity::isDead_srg, "isDead", "Z");
        }

        // --- EntityLivingBase fields ---
        if (ClassResolver::entityLivingBase) {
            auto lb = ClassResolver::entityLivingBase;
            fHealth = GetFieldSafe(env, lb,
                MCP::EntityLivingBase::health_srg, MCP::EntityLivingBase::health_mcp, "F");
            fHurtTime = GetFieldSafe(env, lb,
                MCP::EntityLivingBase::hurtTime_srg, MCP::EntityLivingBase::hurtTime_mcp, "I");

            mGetMaxHealth = env->GetMethodID(lb, MCP::EntityLivingBase::maxHealth_method_srg, "()F");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                mGetMaxHealth = env->GetMethodID(lb, "getMaxHealth", "()F");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }

        // --- Entity.getName() ---
        if (ClassResolver::entity) {
            mGetName = env->GetMethodID(ClassResolver::entity,
                MCP::Entity::getName_srg, "()Ljava/lang/String;");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                mGetName = env->GetMethodID(ClassResolver::entity,
                    "getName", "()Ljava/lang/String;");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }

        s_resolved = true;
        printf("[EntityList] Resolved: playerEntities=%p, listSize=%p, posX=%p, health=%p\n",
            fPlayerEntities, mListSize, fPosX, fHealth);

        return fPlayerEntities != nullptr;
    }

    // Get all players in the world (excluding self)
    static std::vector<EntityInfo> GetPlayers(JNIEnv* env, float maxRange = 64.0f) {
        std::vector<EntityInfo> result;

        jobject player = Minecraft::GetPlayer(env);
        jobject world = Minecraft::GetWorld(env);
        if (!player || !world || !fPlayerEntities || !mListSize || !mListGet)
            return result;

        // Get the List
        jobject list = env->GetObjectField(world, fPlayerEntities);
        if (!list) return result;

        int size = env->CallIntMethod(list, mListSize);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return result; }

        // Player position for distance calc
        double pX = fPosX ? env->GetDoubleField(player, fPosX) : 0;
        double pY = fPosY ? env->GetDoubleField(player, fPosY) : 0;
        double pZ = fPosZ ? env->GetDoubleField(player, fPosZ) : 0;

        for (int i = 0; i < size; i++) {
            jobject entity = env->CallObjectMethod(list, mListGet, i);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!entity) continue;

            // Skip self
            if (env->IsSameObject(entity, player)) continue;

            EntityInfo info = {};
            info.ref = entity;

            // Position
            if (fPosX) info.posX = env->GetDoubleField(entity, fPosX);
            if (fPosY) info.posY = env->GetDoubleField(entity, fPosY);
            if (fPosZ) info.posZ = env->GetDoubleField(entity, fPosZ);

            // Previous position (for interpolation)
            if (fPrevPosX) info.prevPosX = env->GetDoubleField(entity, fPrevPosX);
            if (fPrevPosY) info.prevPosY = env->GetDoubleField(entity, fPrevPosY);
            if (fPrevPosZ) info.prevPosZ = env->GetDoubleField(entity, fPrevPosZ);

            // Distance check
            double dx = info.posX - pX;
            double dy = info.posY - pY;
            double dz = info.posZ - pZ;
            info.distanceToPlayer = std::sqrt(dx*dx + dy*dy + dz*dz);

            if (info.distanceToPlayer > maxRange) continue;

            // Rotation
            if (fYaw) info.yaw = env->GetFloatField(entity, fYaw);
            if (fPitch) info.pitch = env->GetFloatField(entity, fPitch);

            // Health
            if (fHealth) info.health = env->GetFloatField(entity, fHealth);
            if (mGetMaxHealth) {
                info.maxHealth = env->CallFloatMethod(entity, mGetMaxHealth);
                if (env->ExceptionCheck()) { env->ExceptionClear(); info.maxHealth = 20.f; }
            } else {
                info.maxHealth = 20.f;
            }

            // State
            if (fHurtTime) info.hurtTime = env->GetIntField(entity, fHurtTime);
            if (fIsDead) info.isDead = env->GetBooleanField(entity, fIsDead);
            if (fOnGround) info.onGround = env->GetBooleanField(entity, fOnGround);

            // Skip dead entities
            if (info.isDead) continue;
            if (info.health <= 0) continue;

            // Name
            if (mGetName) {
                jstring jName = (jstring)env->CallObjectMethod(entity, mGetName);
                if (env->ExceptionCheck()) { env->ExceptionClear(); }
                else if (jName) {
                    const char* nameChars = env->GetStringUTFChars(jName, nullptr);
                    if (nameChars) {
                        info.name = nameChars;
                        env->ReleaseStringUTFChars(jName, nameChars);
                    }
                }
            }

            result.push_back(info);
        }

        return result;
    }

    // Find closest player to our crosshair (for AimAssist)
    static EntityInfo* FindClosestToCrosshair(JNIEnv* env,
        std::vector<EntityInfo>& entities, float fov, float maxRange)
    {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return nullptr;

        float playerYaw = fYaw ? env->GetFloatField(player, fYaw) : 0;
        float playerPitch = fPitch ? env->GetFloatField(player, fPitch) : 0;

        EntityInfo* best = nullptr;
        float bestAngle = fov / 2.0f;

        for (auto& e : entities) {
            if (e.distanceToPlayer > maxRange) continue;

            // Calculate angle to entity
            auto rot = Minecraft::GetRotationsTo(env, player, e.ref);
            float yawDiff = rot.yaw - playerYaw;
            // Wrap
            while (yawDiff > 180.f) yawDiff -= 360.f;
            while (yawDiff < -180.f) yawDiff += 360.f;
            float pitchDiff = rot.pitch - playerPitch;

            float angle = std::sqrt(yawDiff * yawDiff + pitchDiff * pitchDiff);

            if (angle < bestAngle) {
                bestAngle = angle;
                best = &e;
            }
        }

        return best;
    }

    // Find closest player by distance
    static EntityInfo* FindClosest(std::vector<EntityInfo>& entities, float maxRange) {
        EntityInfo* best = nullptr;
        double bestDist = maxRange;

        for (auto& e : entities) {
            if (e.distanceToPlayer < bestDist) {
                bestDist = e.distanceToPlayer;
                best = &e;
            }
        }

        return best;
    }

    // Find player with lowest health
    static EntityInfo* FindLowestHP(std::vector<EntityInfo>& entities, float maxRange) {
        EntityInfo* best = nullptr;
        float bestHP = 9999.f;

        for (auto& e : entities) {
            if (e.distanceToPlayer > maxRange) continue;
            if (e.health < bestHP) {
                bestHP = e.health;
                best = &e;
            }
        }

        return best;
    }

private:
    static jfieldID GetFieldSafe(JNIEnv* env, jclass cls,
        const char* name1, const char* name2, const char* sig)
    {
        jfieldID f = env->GetFieldID(cls, name1, sig);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            f = env->GetFieldID(cls, name2, sig);
            if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        }
        return f;
    }
};
