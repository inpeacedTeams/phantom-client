#pragma once
#include <vector>
#include <string>
#include <cstdio>

#include "../modules/module_manager.h"
#include "../gui/notifications.h"

// =================================================================
// Presets
// =================================================================
// Opinionated starting points. A preset says three things:
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
// that value.
//
// So Apply counts every entry it could not match and NAMES the
// first few in the notification, and Verify() walks every preset at
// startup without applying anything, so a mismatch is caught before
// anyone presses a preset button in a fight.
//
// The client now ships five modules (Velocity, Sprint Reset, No
// Jump Delay, ESP, Fullbright), so the presets only ever touch
// those.
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
    // LEGIT-SAFE — prediction anticheats (AGC, Grim, Polar)
    //
    // Velocity's legit jump/strafe drives real keybinds, so the
    // packets match a good player. Sprint Reset W-Taps. ESP and
    // Fullbright are client-side rendering the server never sees.
    // No Jump Delay is forced off: jump frequency is a hard vanilla
    // constant and the cheapest thing to check.
    // ---------------------------------------------------------
    static Profile Legit() {
        return {
            "Legit",
            "Safe on prediction anticheats. Velocity's legit modes plus a "
            "W-Tap sprint reset, nothing that rewrites motion or reach.",
            { "Velocity", "Sprint Reset", "ESP", "Fullbright" },
            { "No Jump Delay" },
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
                { "Sprint Reset", "Method",               0 },   // W-Tap
                { "Sprint Reset", "Chance",              85 },
                { "Sprint Reset", "Hold Min",             1 },
                { "Sprint Reset", "Hold Max",             2 },
                { "Sprint Reset", "Only On Landed Hits",  1 },
                { "Sprint Reset", "Require Sprint",       1 },
                { "Sprint Reset", "Re-arm Sprint",        1 },

                // ---- ESP: quiet ----
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
    // BEDWARS — a team game rather than a duel
    //
    // Same safe combat as Legit, but ESP is louder because knowing
    // who is where across the map is worth more than a clean screen.
    // ---------------------------------------------------------
    static Profile Bedwars() {
        return {
            "Bedwars",
            "Team games. Legit combat with a louder ESP: names, health and "
            "a longer range so you can read the whole fight.",
            { "Velocity", "Sprint Reset", "ESP", "Fullbright" },
            { "No Jump Delay" },
            {
                { "Velocity", "Mode",           5 },
                { "Velocity", "Jump Chance",   75 },
                { "Velocity", "Strafe Chance", 70 },
                { "Velocity", "Toward Attacker", 1 },

                { "Sprint Reset", "Method",              0 },
                { "Sprint Reset", "Chance",             92 },
                { "Sprint Reset", "Only On Landed Hits", 1 },

                { "ESP", "Range",    64 },
                { "ESP", "Name",      1 },
                { "ESP", "Health",    1 },
                { "ESP", "Distance",  1 },
                { "ESP", "Tracers",   0 },
            }
        };
    }

    // ---------------------------------------------------------
    // MINIMAL — nothing that touches the game at all
    //
    // Just the visual helpers. Survives a spectator and a recording
    // because there is no input or motion to see.
    // ---------------------------------------------------------
    static Profile Minimal() {
        return {
            "Minimal",
            "Visual only. ESP and Fullbright, no combat or movement help at "
            "all, so there is nothing for anyone to catch.",
            { "ESP", "Fullbright" },
            { "Velocity", "Sprint Reset", "No Jump Delay" },
            {
                { "ESP", "Range",    56 },
                { "ESP", "Name",      1 },
                { "ESP", "Health",    1 },
                { "ESP", "Tracers",   0 },
            }
        };
    }

    // ---------------------------------------------------------
    // BLATANT — unprotected or vanilla servers only. Everything
    // here is caught instantly by any real anticheat.
    // ---------------------------------------------------------
    static Profile Blatant() {
        return {
            "Blatant",
            "Servers with no anticheat at all. Direct velocity cancel and no "
            "jump delay are caught instantly anywhere else.",
            { "Velocity", "No Jump Delay", "ESP", "Fullbright" },
            { "Sprint Reset" },
            {
                { "Velocity", "Mode", 1 },   // Cancel

                { "ESP", "Range",    80 },
                { "ESP", "Tracers",   1 },
                { "ESP", "Distance",  1 },
                { "ESP", "Name",      1 },
                { "ESP", "Health",    1 },
            }
        };
    }

    static std::vector<Profile> All() {
        return { Legit(), Bedwars(), Minimal(), Blatant() };
    }

    // ---------------------------------------------------------
    // Verify
    //
    // Walks every preset and checks that each module and setting it
    // names still exists, WITHOUT changing anything. Called once at
    // startup so a rename is caught immediately.
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
