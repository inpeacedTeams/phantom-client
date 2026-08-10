#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"

// =================================================================
// No Jump Delay
// =================================================================
// Vanilla forces a 10-tick wait between jumps via
// EntityLivingBase.jumpTicks. Zeroing it every tick removes that.
//
// Worth knowing: jump frequency is one of the cheapest things for a
// prediction anticheat to check, because the vanilla minimum is a
// hard constant. This is safe on unprotected servers and nowhere
// else.
//
// There is nothing to configure here and no setting has been
// invented to make the panel look busier. The panel says what it
// does and what it costs you.
// =================================================================

class NoJumpDelay : public Module {
private:
    jfieldID m_fJumpTicks = nullptr;
    bool m_resolved = false;

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;
        if (ClassResolver::entityLivingBase) {
            m_fJumpTicks = JvmtiUtil::FindField(env, ClassResolver::entityLivingBase,
                { "field_70773_bE", "jumpTicks" });
        }
        m_resolved = true;
    }

public:
    NoJumpDelay() : Module("No Jump Delay", "Remove the cooldown between jumps",
                           ModuleCategory::MOVEMENT, 0) {}

    void OnTick(JNIEnv* env) override {
        Resolve(env);
        if (!m_fJumpTicks) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        env->SetIntField(player, m_fJumpTicks, 0);
    }

    // The field id belongs to a class, not a world, so it survives a
    // reconnect. It does NOT survive the class never having been
    // resolved, which is the case if the module was enabled before
    // the game finished loading, so a reset is a free second try.
    void OnReset(JNIEnv*) override {
        if (!m_fJumpTicks) m_resolved = false;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!m_fJumpTicks) {
            *text = "The jump cooldown field could not be found in this build "
                    "of the game, so the module cannot do anything.";
            return NoticeLevel::Warning;
        }
        *text = "Jump frequency has a hard vanilla minimum, so this is one of "
                "the cheapest things for an anticheat to check. Unprotected "
                "servers only.";
        return NoticeLevel::Danger;
    }
};
