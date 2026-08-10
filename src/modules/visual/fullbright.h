#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <cstdio>

// =================================================================
// Fullbright
// =================================================================
// Raises GameSettings.gammaSetting far past the slider maximum so
// dark areas render fully lit. Purely client-side rendering state,
// so no server ever sees it.
//
// THE TRAP THIS AVOIDS
// Disabling has to put the player's own brightness back, which
// means capturing it on enable. But the vanilla slider only goes
// from 0 to 1, so any value above that is not a brightness the
// player chose: it is OUR value, still in place because the last
// session ended without a clean disable.
//
// Saving that as "the original" is how a client leaves someone with
// a permanently washed-out game and no obvious way to fix it, so
// anything out of the vanilla range is treated as the default
// instead.
//
// The setting used to be stored as "Gamma" and drawn as
// "Brightness". Two names for one value is how a preset ends up
// setting something that does not exist.
// =================================================================

class Fullbright : public Module {
private:
    float m_brightness = 100.0f;

    float m_savedGamma = 1.0f;
    bool  m_saved = false;

    mutable char m_status[32] = {};

    // The real slider range in 1.8: Moody 0 to Bright 1.
    static constexpr float kVanillaMax = 1.0f;
    static constexpr float kDefault    = 1.0f;

public:
    Fullbright() : Module("Fullbright", "See in complete darkness",
                          ModuleCategory::VISUAL, 'H')
    {
        Bind("Brightness", &m_brightness, 1.0f, 1000.0f, "%.0f",
             "Past about 100 there is no further difference: the world is "
             "already fully lit");
    }

    void OnEnable(JNIEnv* env) override {
        float current = Minecraft::GetGamma(env);

        // Out of the vanilla range means it is ours from a previous
        // session, not theirs.
        if (current < 0.0f || current > kVanillaMax) current = kDefault;

        m_savedGamma = current;
        m_saved = true;
    }

    void OnTick(JNIEnv* env) override {
        // Reapplied every tick because the options screen writes the
        // slider value back over ours whenever it is open.
        Minecraft::SetGamma(env, m_brightness);
    }

    void OnDisable(JNIEnv* env) override {
        if (!env) return;
        Minecraft::SetGamma(env, m_saved ? m_savedGamma : kDefault);
        m_status[0] = '\0';
    }

    const char* StatusLine() const override {
        snprintf(m_status, sizeof(m_status), "%.0f", m_brightness);
        return m_status;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!m_saved) return NoticeLevel::None;
        *text = "Your own brightness is remembered and put back when this "
                "goes off.";
        return NoticeLevel::Info;
    }
};
