#pragma once
#include <vector>
#include <string>
#include <cstdio>

#include "../modules/module_manager.h"
#include "../gui/notifications.h"

// =================================================================
// Presets
// =================================================================
// Opinionated starting points, one per anticheat. A preset says
// three things:
//
//   enable    modules to turn on
//   disable   modules to force off
//   values    setting overrides, module + setting + value
//
// Anything not mentioned is left exactly as the user had it.
//
// -----------------------------------------------------------------
// WHY THIS FILE IS DANGEROUS AND HOW THAT IS HANDLED
// -----------------------------------------------------------------
// Every entry here is a STRING that has to match a name a module
// registered with Bind(). Rename a setting in a module and the
// compiler says nothing; the preset just quietly stops applying
// that value, and the user gets a config that is subtly wrong in a
// way nobody can see.
//
// That has now happened twice. So Apply counts every entry it could
// not match and NAMES the first few in the notification, because a
// count alone tells you something is wrong but not what, and the
// console it used to print to does not exist in a release build.
//
// There is also a Verify() that walks every preset without applying
// anything, so a mismatch can be caught at startup instead of the
// first time somebody presses a preset button.
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

public:
    // ---------------------------------------------------------
    // MINEMEN CLUB — AntiGamingChair, built on Karhu
    //
    // A prediction anticheat on a low-ping duel server. It
    // simulates your movement and compares, and runs statistics on
    // click intervals and rotation deltas. Nothing that touches
    // motion or reach survives here, so this preset is entirely
    // keyboard simulation plus client-side rendering.
    // ---------------------------------------------------------
    static Profile Minemen() {
        return {
            "Minemen",
            "Duel servers running AGC. Keyboard-level only: nothing here "
            "touches your motion or your reach.",
            { "Velocity", "Sprint Reset", "Auto Blockhit", "Hit Select",
              "AutoClicker", "Sprint", "Speed", "ESP", "Fullbright" },
            { "Fly", "Kill Aura", "No Jump Delay", "Backtrack", "Aim Assist" },
            {
                // ---- Velocity: legit modes only ----
                { "Velocity", "Mode",             5 },   // Both
                { "Velocity", "Jump Chance",     65 },
                { "Velocity", "Jump Delay Min",   0 },
                { "Velocity", "Jump Delay Max",   2 },
                { "Velocity", "Hits Until Jump",  1 },
                { "Velocity", "Strafe Chance",   60 },
                { "Velocity", "Strafe Delay",     1 },
                { "Velocity", "Strafe Ticks",     2 },
                { "Velocity", "Toward Attacker",  1 },
                { "Velocity", "Only In Combat",   1 },

                // ---- Sprint Reset ----
                // W-Tap. Never Ctrl here: toggling the sprint key is
                // the one variant AGC watches for.
                { "Sprint Reset", "Method",               0 },
                { "Sprint Reset", "Chance",              88 },
                { "Sprint Reset", "Hold Min",             1 },
                { "Sprint Reset", "Hold Max",             2 },
                { "Sprint Reset", "Hit Delay",            0 },
                { "Sprint Reset", "Only On Landed Hits",  1 },
                { "Sprint Reset", "Require Sprint",       1 },
                { "Sprint Reset", "Re-arm Sprint",        1 },

                // ---- Auto Blockhit ----
                // Predict their swing and block around it. Coverage
                // is a consequence of how often they attack, not a
                // number we set.
                { "Auto Blockhit", "Mode",             0 },   // Predict
                { "Auto Blockhit", "Reach",          3.6f },
                { "Auto Blockhit", "Detect Range",   6.0f },
                { "Auto Blockhit", "Lead",           120 },
                { "Auto Blockhit", "Reaction Min",    40 },
                { "Auto Blockhit", "Reaction Max",   100 },
                { "Auto Blockhit", "Swing Gap",        1 },
                { "Auto Blockhit", "After Hit",        2 },
                { "Auto Blockhit", "Max Hold",        12 },
                { "Auto Blockhit", "Chance",          92 },
                { "Auto Blockhit", "Timing Noise",    30 },
                { "Auto Blockhit", "Only Sword",       1 },
                { "Auto Blockhit", "Protect Movement", 1 },

                // ---- AutoClicker ----
                // 13 is a rate a good player reaches by hand. The
                // drift model is what keeps the stream from looking
                // generated, so it stays on.
                { "AutoClicker", "CPS",           13 },
                { "AutoClicker", "Variance",      22 },
                { "AutoClicker", "Drift",          1 },
                { "AutoClicker", "Drift Amount",  14 },
                { "AutoClicker", "Fumbles",        1 },
                { "AutoClicker", "Fumble Chance",  4 },
                { "AutoClicker", "Bursts",         1 },
                { "AutoClicker", "Burst Chance",   4 },
                { "AutoClicker", "Floor",         26 },

                { "Hit Select", "Chance",     75 },
                { "Sprint",     "Omni Sprint", 0 },

                // ---- Speed ----
                // Sprint jump only, and it stands down during an
                // exchange so Sprint Reset keeps the ground it needs.
                { "Speed", "Mode",             0 },
                { "Speed", "Require Sprint",   1 },
                { "Speed", "Forward Only",     1 },
                { "Speed", "Pause After A Hit", 1 },
                { "Speed", "Combat Pause",     8 },
                { "Speed", "Skip Chance",      9 },
                { "Speed", "Ground Ticks Min", 1 },
                { "Speed", "Ground Ticks Max", 3 },

                // ---- ESP ----
                // Quiet. A duel is one opponent; names and tracers
                // are clutter you do not need.
                { "ESP", "Range",     48 },
                { "ESP", "Box Style",  0 },
                { "ESP", "Name",       0 },
                { "ESP", "Tracers",    0 },
                { "ESP", "Health",     1 },
                { "ESP", "Distance",   0 },
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
            "MineBlaze, Pika, GommeHD. Legit combat plus bridging, tuned "
            "for BedWars rather than duels.",
            { "Velocity", "Sprint Reset", "Auto Blockhit", "Aim Assist",
              "AutoClicker", "Bridge Assist", "Sprint", "Speed",
              "ESP", "Fullbright" },
            { "Fly", "Kill Aura", "Backtrack" },
            {
                { "Velocity", "Mode",            5 },
                { "Velocity", "Jump Chance",    75 },
                { "Velocity", "Strafe Chance",  70 },
                { "Velocity", "Toward Attacker", 1 },

                { "Sprint Reset", "Method",              0 },
                { "Sprint Reset", "Chance",             92 },
                { "Sprint Reset", "Hold Min",            1 },
                { "Sprint Reset", "Hold Max",            2 },
                { "Sprint Reset", "Only On Landed Hits", 1 },

                // Slightly earlier block than on AGC: Polar cares
                // less about the block cadence itself.
                { "Auto Blockhit", "Mode",          0 },
                { "Auto Blockhit", "Reach",      3.7f },
                { "Auto Blockhit", "Lead",        150 },
                { "Auto Blockhit", "Chance",       96 },
                { "Auto Blockhit", "Timing Noise", 26 },
                { "Auto Blockhit", "Only Sword",    1 },

                // Aim assist is usable on Polar, but stay slow.
                { "Aim Assist", "Speed",                2.4f },
                { "Aim Assist", "Pitch Ratio",          0.55f },
                { "Aim Assist", "Smoothing",            0.6f },
                { "Aim Assist", "FOV",                   70 },
                { "Aim Assist", "Range",                3.4f },
                { "Aim Assist", "Only While Swinging",    1 },
                { "Aim Assist", "Jitter",                18 },
                { "Aim Assist", "Wander",               0.22f },
                { "Aim Assist", "Breaks",                 1 },
                { "Aim Assist", "Break Chance",          12 },

                { "AutoClicker", "CPS",      12 },
                { "AutoClicker", "Variance", 20 },
                { "AutoClicker", "Drift",     1 },

                // Bridge Assist pace: 1 is Balanced, and the three
                // timings below are exactly its numbers, so the
                // preset does not immediately flip to Custom.
                { "Bridge Assist", "Pace",                        1 },
                { "Bridge Assist", "Edge Distance",           0.30f },
                { "Bridge Assist", "Release Delay",               1 },
                { "Bridge Assist", "Press Delay",                 2 },
                { "Bridge Assist", "Only While Walking Backwards", 1 },

                // Hopping across a bedwars map is most of the value
                { "Speed", "Mode",             0 },
                { "Speed", "Require Sprint",   1 },
                { "Speed", "Forward Only",     1 },
                { "Speed", "Pause After A Hit", 1 },
                { "Speed", "Combat Pause",     6 },
                { "Speed", "Skip Chance",      7 },

                // A team game: knowing who is where matters more
                // than keeping the screen clean.
                { "ESP", "Range",    64 },
                { "ESP", "Name",      1 },
                { "ESP", "Health",    1 },
                { "ESP", "Tracers",   0 },
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
            "The minimum. Nothing here produces an input a good player "
            "could not, so it survives a spectator and a recording.",
            { "Sprint Reset", "Auto Blockhit", "Sprint", "Speed", "Fullbright" },
            { "Velocity", "Aim Assist", "Kill Aura", "Fly",
              "Backtrack", "AutoClicker", "ESP", "No Jump Delay",
              "Hit Select" },
            {
                { "Sprint Reset", "Method",              0 },
                { "Sprint Reset", "Chance",             70 },
                { "Sprint Reset", "Only On Landed Hits", 1 },

                // Reactive only: blocks while they are visibly
                // mid-swing and never guesses. On a recording it
                // reads as someone with good reactions.
                { "Auto Blockhit", "Mode",             1 },
                { "Auto Blockhit", "Reach",          3.4f },
                { "Auto Blockhit", "Chance",          75 },
                { "Auto Blockhit", "After Hit",        3 },
                { "Auto Blockhit", "Only Sword",       1 },
                { "Auto Blockhit", "Protect Movement", 1 },

                // Loose hop rhythm. Someone watching should see a
                // player bunny hopping, not a metronome.
                { "Speed", "Mode",              0 },
                { "Speed", "Require Sprint",    1 },
                { "Speed", "Forward Only",      1 },
                { "Speed", "Pause After A Hit", 1 },
                { "Speed", "Combat Pause",     10 },
                { "Speed", "Skip Chance",      16 },
                { "Speed", "Ground Ticks Min",  1 },
                { "Speed", "Ground Ticks Max",  4 },
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
            "Servers with no anticheat at all. Every one of these is caught "
            "instantly anywhere else.",
            { "Kill Aura", "Velocity", "Auto Blockhit", "Speed", "Sprint",
              "ESP", "Fullbright", "No Jump Delay", "AutoClicker" },
            { "Backtrack" },
            {
                { "Velocity", "Mode", 1 },   // Cancel

                { "Kill Aura", "Range",            4.2f },
                { "Kill Aura", "Min CPS",            14 },
                { "Kill Aura", "Max CPS",            18 },
                { "Kill Aura", "Rotate To Target",    1 },

                // Nothing to hide from, so hold the block whenever
                // anyone is close enough to swing at you.
                { "Auto Blockhit", "Mode",             2 },   // In Range
                { "Auto Blockhit", "Reach",          4.0f },
                { "Auto Blockhit", "Chance",         100 },
                { "Auto Blockhit", "Protect Movement", 1 },

                // The only preset that asks for raw motion
                { "Speed", "Mode",        1 },   // Strafe
                { "Speed", "Multiplier", 1.6f },
                { "Speed", "Ground Only", 0 },

                { "AutoClicker", "CPS",      18 },
                { "AutoClicker", "Variance", 10 },
                { "AutoClicker", "Floor",    20 },

                { "ESP", "Tracers",  1 },
                { "ESP", "Distance", 1 },
                { "ESP", "Name",     1 },
            }
        };
    }

    static std::vector<Profile> All() {
        return { Minemen(), Polar(), Legit(), Blatant() };
    }

    // ---------------------------------------------------------
    // Verify
    //
    // Walks every preset and checks that each module and setting it
    // names still exists, WITHOUT changing anything. Called once at
    // startup so a rename is caught immediately rather than the
    // first time somebody presses a preset button in a fight.
    //
    // Returns the number of broken entries and describes the first
    // few, which is all anyone needs to go and fix it.
    // ---------------------------------------------------------
    static int Verify(std::string* detail) {
        int broken = 0;
        std::string firstFew;

        auto note = [&](const std::string& what) {
            broken++;
            if (broken > 3) return;
            if (!firstFew.empty()) firstFew += ", ";
            firstFew += what;
        };

        for (const auto& p : All()) {
            for (const char* n : p.enable)
                if (!ModuleManager::Find(n)) note(std::string(p.name) + ": " + n);
            for (const char* n : p.disable)
                if (!ModuleManager::Find(n)) note(std::string(p.name) + ": " + n);

            for (const auto& v : p.values) {
                Module* m = ModuleManager::Find(v.module);
                if (!m) { note(std::string(v.module)); continue; }

                bool found = false;
                for (const auto& s : m->GetSettings()) {
                    if (s.name == v.setting) { found = true; break; }
                }
                if (!found)
                    note(std::string(v.module) + "." + v.setting);
            }
        }

        if (detail) *detail = firstFew;
        return broken;
    }

    // ---------------------------------------------------------
    // Apply
    //
    // Must run on the client thread: enabling a module calls
    // OnEnable, which needs a live JNIEnv.
    // ---------------------------------------------------------
    static void Apply(const Profile& p, JNIEnv* env) {
        int applied = 0, missing = 0;
        std::string firstMisses;

        for (const char* name : p.disable) {
            Module* m = ModuleManager::Find(name);
            if (m) m->SetEnabled(false, env);
            else   missing++;
        }

        // Values are written BEFORE the enables, so a module that
        // reads its settings in OnEnable sees this preset's values
        // rather than whatever was left from the last one.
        for (const auto& v : p.values) {
            Module* m = ModuleManager::Find(v.module);
            if (m && m->SetValue(v.setting, v.value)) {
                applied++;
                continue;
            }

            missing++;
            // Name the first few. A count alone tells you something
            // is wrong but not what, and this is the only channel
            // that reaches anyone.
            if (missing <= 3) {
                if (!firstMisses.empty()) firstMisses += ", ";
                firstMisses += std::string(v.module) + "." + v.setting;
            }
        }

        for (const char* name : p.enable) {
            Module* m = ModuleManager::Find(name);
            if (m) m->SetEnabled(true, env);
            else   missing++;
        }

        char buf[220];
        if (missing > 0) {
            snprintf(buf, sizeof(buf),
                     "%d values applied, %d could not be matched (%s%s)",
                     applied, missing, firstMisses.c_str(),
                     missing > 3 ? ", ..." : "");
            s_lastReport = buf;
            iOS::Notify::Warning(std::string(p.name) + " applied with gaps",
                                 s_lastReport);
        } else {
            snprintf(buf, sizeof(buf), "%d values applied", applied);
            s_lastReport = buf;
        }
    }

    static const std::string& LastReport() { return s_lastReport; }
};
