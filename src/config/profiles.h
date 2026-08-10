#pragma once
#include <vector>
#include <string>
#include <cstdio>

#include "../modules/module_manager.h"

// =================================================================
// Config profiles
// =================================================================
// Applies real values through Module::SetValue, matching the names
// each module registers with Bind() in its constructor.
//
// A profile says three things:
//   enable    modules to turn on
//   disable   modules to force off
//   values    setting overrides, module + setting + value
//
// Anything not mentioned is left exactly as the user had it.
//
// A misspelt name is NOT silently ignored: Apply counts unmatched
// entries and the Configs tab reports the total. That check is the
// only reason anyone noticed this file had drifted: the modules
// were rewritten, settings were renamed, and a third of every
// profile had quietly stopped applying.
// =================================================================

struct ProfileValue {
    const char* module;
    const char* setting;
    float       value;
};

struct Profile {
    const char* name;
    const char* description;
    std::vector<const char*>  enable;
    std::vector<const char*>  disable;
    std::vector<ProfileValue> values;
};

class Profiles {
private:
    inline static std::string s_lastReport;

    static Module* Find(const char* name) {
        for (auto& m : ModuleManager::GetModules())
            if (m->GetName() == name) return m.get();
        return nullptr;
    }

public:
    // ---------------------------------------------------------
    // MINEMEN CLUB — AntiGamingChair, built on Karhu
    //
    // A prediction anticheat on a low-ping duel server. It
    // simulates your movement and compares, and runs statistics on
    // click intervals and rotation deltas. Nothing that touches
    // motion or reach survives here, so this profile is entirely
    // keyboard simulation plus client-side rendering.
    // ---------------------------------------------------------
    static Profile Minemen() {
        return {
            "Minemen (AGC)",
            "Duel server, prediction AC. Keyboard-level only.",
            // enable
            { "Velocity", "Sprint Reset", "Auto Blockhit", "Hit Select",
              "AutoClicker", "Sprint", "Speed", "ESP", "Fullbright" },
            // disable: movement prediction catches every one of these
            { "Fly", "Kill Aura", "No Jump Delay", "Backtrack",
              "Aim Assist" },
            {
                // Velocity: legit only
                { "Velocity", "Mode",              5 },   // Combined
                { "Velocity", "Jump Chance",      65 },
                { "Velocity", "Jump Delay Min",    0 },
                { "Velocity", "Jump Delay Max",    2 },
                { "Velocity", "Hits Until Jump",   1 },
                { "Velocity", "Strafe Chance",    60 },
                { "Velocity", "Strafe Delay",      1 },
                { "Velocity", "Strafe Ticks",      2 },
                { "Velocity", "Strafe Toward",     1 },

                // Sprint Reset: W-Tap. Never Ctrl here, the sprint
                // key toggling is the one variant AGC watches.
                { "Sprint Reset", "Method",           0 },
                { "Sprint Reset", "Chance",          88 },
                { "Sprint Reset", "Reset Ticks Min",  1 },
                { "Sprint Reset", "Reset Ticks Max",  2 },
                { "Sprint Reset", "Hit Delay",        0 },
                { "Sprint Reset", "Only On Hit",      1 },

                // Auto Blockhit: predict their swing and block around
                // it. Coverage is a consequence of how often they
                // attack, not a number we set.
                { "Auto Blockhit", "Mode",             0 },   // Predict
                { "Auto Blockhit", "Block Range",    3.6f },
                { "Auto Blockhit", "Detect Range",   6.0f },
                { "Auto Blockhit", "Lead",           120 },
                { "Auto Blockhit", "Reaction Min",    40 },
                { "Auto Blockhit", "Reaction Max",   100 },
                { "Auto Blockhit", "Swing Gap",        1 },
                { "Auto Blockhit", "After Hit",        2 },
                { "Auto Blockhit", "Max Block Ticks", 12 },
                { "Auto Blockhit", "Chance",          92 },
                { "Auto Blockhit", "Timing Noise",    30 },
                { "Auto Blockhit", "Only Sword",       1 },
                { "Auto Blockhit", "Protect Movement", 1 },

                // AutoClicker. 13 is a rate a good player reaches by
                // hand; the drift model is what keeps the stream from
                // looking generated.
                { "AutoClicker", "CPS",           13 },
                { "AutoClicker", "Variance",      22 },
                { "AutoClicker", "Drift",          1 },
                { "AutoClicker", "Drift Amount",  14 },
                { "AutoClicker", "Fumbles",        1 },
                { "AutoClicker", "Fumble Chance",  4 },
                { "AutoClicker", "Bursts",         1 },
                { "AutoClicker", "Burst Chance",   4 },
                { "AutoClicker", "Floor",         26 },

                { "Hit Select", "Chance",  75 },
                { "Sprint", "Omni Sprint",  0 },

                // Sprint jump only, and it stands down during an
                // exchange so More KB keeps the ground it needs.
                { "Speed", "Mode",             0 },
                { "Speed", "Require Sprint",   1 },
                { "Speed", "Forward Only",     1 },
                { "Speed", "Pause In Combat",  1 },
                { "Speed", "Combat Pause",     8 },
                { "Speed", "Skip Chance",      9 },
                { "Speed", "Ground Ticks Min", 1 },
                { "Speed", "Ground Ticks Max", 3 },

                { "ESP", "Max Range",     48 },
                { "ESP", "Show Name",      0 },
                { "ESP", "Show Tracers",   0 },
                { "ESP", "Box Style",      0 },
            }
        };
    }

    // ---------------------------------------------------------
    // POLAR — MineBlaze, Pika, GommeHD, MasedWorld
    //
    // Less strict on movement than AGC, but very good at direct
    // velocity edits and rotation snapping. Bridge assist is safe
    // here and worth having for BedWars.
    // ---------------------------------------------------------
    static Profile Polar() {
        return {
            "Polar",
            "MineBlaze, Pika, GommeHD. Legit combat plus bridging.",
            { "Velocity", "Sprint Reset", "Auto Blockhit", "Aim Assist",
              "AutoClicker", "Bridge Assist", "Sprint", "Speed",
              "ESP", "Fullbright" },
            { "Fly", "Kill Aura", "Backtrack" },
            {
                { "Velocity", "Mode",             5 },
                { "Velocity", "Jump Chance",     75 },
                { "Velocity", "Strafe Chance",   70 },

                { "Sprint Reset", "Method",          0 },
                { "Sprint Reset", "Chance",         92 },
                { "Sprint Reset", "Reset Ticks Min", 1 },
                { "Sprint Reset", "Reset Ticks Max", 2 },

                // Slightly earlier block than on AGC: Polar cares
                // less about the block cadence itself.
                { "Auto Blockhit", "Mode",            0 },
                { "Auto Blockhit", "Block Range",   3.7f },
                { "Auto Blockhit", "Lead",          150 },
                { "Auto Blockhit", "Chance",         96 },
                { "Auto Blockhit", "Timing Noise",   26 },
                { "Auto Blockhit", "Only Sword",      1 },

                // Aim assist is usable on Polar, but stay slow.
                // These names track the rewritten module: one Speed,
                // with pitch expressed as a ratio of it.
                { "Aim Assist", "Speed",         2.4f },
                { "Aim Assist", "Pitch Ratio",   0.55f },
                { "Aim Assist", "Smoothing",     0.6f },
                { "Aim Assist", "FOV",            70 },
                { "Aim Assist", "Range",         3.4f },
                { "Aim Assist", "Require Swing",   1 },
                { "Aim Assist", "Jitter",         18 },
                { "Aim Assist", "Wander",        0.22f },
                { "Aim Assist", "Breaks",          1 },
                { "Aim Assist", "Break Chance",   12 },

                { "AutoClicker", "CPS",          12 },
                { "AutoClicker", "Variance",     20 },
                { "AutoClicker", "Drift",         1 },

                // Bridge Assist pace: 1 is Balanced
                { "Bridge Assist", "Mode",           1 },
                { "Bridge Assist", "Edge Distance", 0.30f },
                { "Bridge Assist", "Release Delay",  1 },
                { "Bridge Assist", "Press Delay",    2 },

                // Hopping across a bedwars map is most of the value
                { "Speed", "Mode",             0 },
                { "Speed", "Require Sprint",   1 },
                { "Speed", "Forward Only",     1 },
                { "Speed", "Pause In Combat",  1 },
                { "Speed", "Combat Pause",     6 },
                { "Speed", "Skip Chance",      7 },
            }
        };
    }

    // ---------------------------------------------------------
    // LEGIT — safe on anything, including recording and staff
    // spectating. Nothing here produces a packet a good player
    // would not.
    // ---------------------------------------------------------
    static Profile Legit() {
        return {
            "Legit",
            "Minimum values. Safe under spectator and on video.",
            { "Sprint Reset", "Auto Blockhit", "Sprint", "Speed", "Fullbright" },
            { "Velocity", "Aim Assist", "Kill Aura", "Fly",
              "Backtrack", "AutoClicker", "ESP", "No Jump Delay" },
            {
                { "Sprint Reset", "Method",  0 },
                { "Sprint Reset", "Chance", 70 },

                // Reactive only: blocks while they are visibly
                // mid-swing and never guesses. On a recording it
                // reads as someone with good reactions.
                { "Auto Blockhit", "Mode",          1 },
                { "Auto Blockhit", "Block Range", 3.4f },
                { "Auto Blockhit", "Chance",       75 },
                { "Auto Blockhit", "After Hit",     3 },
                { "Auto Blockhit", "Only Sword",    1 },

                // Loose hop rhythm. Someone watching the recording
                // should see a player bunny hopping, not a metronome.
                { "Speed", "Mode",             0 },
                { "Speed", "Require Sprint",   1 },
                { "Speed", "Forward Only",     1 },
                { "Speed", "Pause In Combat",  1 },
                { "Speed", "Combat Pause",    10 },
                { "Speed", "Skip Chance",     16 },
                { "Speed", "Ground Ticks Min", 1 },
                { "Speed", "Ground Ticks Max", 4 },
            }
        };
    }

    // ---------------------------------------------------------
    // BLATANT — unprotected or vanilla servers only. Everything
    // in here is caught instantly by any real anticheat.
    // ---------------------------------------------------------
    static Profile Blatant() {
        return {
            "Blatant",
            "No-anticheat servers only. Detected everywhere else.",
            { "Kill Aura", "Velocity", "Auto Blockhit", "Speed", "Sprint",
              "ESP", "Fullbright", "No Jump Delay", "AutoClicker" },
            { "Backtrack" },
            {
                { "Velocity", "Mode",       1 },   // Cancel
                { "Kill Aura", "Range",   4.2f },
                { "Kill Aura", "Min CPS",  14 },
                { "Kill Aura", "Max CPS",  18 },

                // Nothing to hide from, so just hold it whenever
                // anyone is close enough to swing at you.
                { "Auto Blockhit", "Mode",         2 },   // In Range
                { "Auto Blockhit", "Block Range",  4.0f },
                { "Auto Blockhit", "Chance",     100 },

                // The only profile that asks for raw motion
                { "Speed", "Mode",         1 },   // Strafe
                { "Speed", "Multiplier", 1.6f },
                { "Speed", "Ground Only",  0 },

                { "AutoClicker", "CPS",       18 },
                { "AutoClicker", "Variance",  10 },
                { "AutoClicker", "Floor",     20 },

                { "ESP", "Show Tracers",   1 },
                { "ESP", "Show Distance",  1 },
            }
        };
    }

    static std::vector<Profile> All() {
        return { Minemen(), Polar(), Legit(), Blatant() };
    }

    // ---------------------------------------------------------
    // Apply. Must run on the client thread: enabling a module
    // calls OnEnable, which needs a live JNIEnv.
    // ---------------------------------------------------------
    static void Apply(const Profile& p, JNIEnv* env) {
        int applied = 0, missing = 0;

        for (const char* name : p.disable) {
            Module* m = Find(name);
            if (m) m->SetEnabled(false, env);
        }

        // Values are written BEFORE the enables, so a module that
        // reads its settings in OnEnable sees the profile's values
        // rather than whatever was left over from the last one.
        for (const auto& v : p.values) {
            Module* m = Find(v.module);
            if (m && m->SetValue(v.setting, v.value)) {
                applied++;
            } else {
                missing++;
                printf("[Profile] unknown setting: %s / %s\n", v.module, v.setting);
            }
        }

        for (const char* name : p.enable) {
            Module* m = Find(name);
            if (m) m->SetEnabled(true, env);
        }

        char buf[160];
        if (missing > 0) {
            snprintf(buf, sizeof(buf), "%s: %d applied, %d UNMATCHED (see console)",
                     p.name, applied, missing);
        } else {
            snprintf(buf, sizeof(buf), "%s applied, %d values", p.name, applied);
        }
        s_lastReport = buf;
        printf("[Profile] %s\n", buf);
    }

    static const std::string& LastReport() { return s_lastReport; }
};
