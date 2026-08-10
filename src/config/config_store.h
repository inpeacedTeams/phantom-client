#pragma once
#include <Windows.h>
#include <ShlObj.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>

#include "../modules/module_manager.h"
#include "../gui/ios_theme.h"
#include "../gui/ios_hud.h"

// =================================================================
// ConfigStore
// =================================================================
// Until recently nothing in this client survived an eject. Every
// setting you touched, every keybind you set, every module you
// turned on: gone the moment the DLL unloaded. That is the single
// biggest gap between "a set of working features" and "a client you
// use".
//
// WHERE
//   %APPDATA%\Phantom\
//     default.cfg      loaded at startup, written at eject
//     <name>.cfg       whatever the user saves
//
// APPDATA rather than next to the DLL: the DLL usually lives in a
// download folder, and writing beside it fails outright under
// Program Files.
//
// FORMAT
// Flat text, one key per line. Not JSON, because a JSON parser is
// three hundred lines to gain nothing here, and a config a human
// can open and fix by hand is worth more than a tidy one they
// cannot.
//
//     # Phantom config v2
//     ui.scale=1.00
//     ui.accent=2
//     hud.wmX=0.012
//     module.AutoClicker.enabled=1
//     module.AutoClicker.key=82
//     module.AutoClicker.set.CPS=13.000000
//
// A colour setting is the one value made of more than one number.
// It is stored as four suffixed keys, exactly the way ui.accentCustom
// already is, rather than packed into a single float: a packed 24-
// or 32-bit colour prints as 1.67e+07 under the default ostream
// formatting and reparses to the wrong colour.
//
//     module.ESP.set.Box Colour.r=0.000000
//     module.ESP.set.Box Colour.g=0.480000
//     module.ESP.set.Box Colour.b=1.000000
//     module.ESP.set.Box Colour.a=1.000000
//
// FORWARD AND BACKWARD COMPATIBILITY, WHICH IS THE WHOLE POINT
//
//   Unknown keys are IGNORED, not treated as an error. A config
//   written by a newer build loads fine on an older one.
//
//   Missing keys keep the module's own default. Adding a setting
//   never invalidates an existing config, so nobody has to delete
//   their setup because the client gained a feature.
//
//   A malformed line is skipped and counted, never fatal. A config
//   truncated by a crash mid-write still loads everything up to
//   the break.
//
//   A key that has been REPLACED is migrated rather than dropped.
//   hud.corner is the first of those: the HUD used to be one of
//   four corner presets and is now a free position, so an old
//   config is translated into the equivalent coordinates instead
//   of silently resetting someone's layout.
//
// Because every module setting is already registered through
// Bind(), modules need no per-module code here at all: a new module
// is saved correctly the day it is written.
// =================================================================

class ConfigStore {
public:
    struct Report {
        int  applied = 0;
        int  unknown = 0;
        int  malformed = 0;
        int  migrated = 0;
        bool ok = false;
        std::string message;
    };

private:
    inline static std::string s_dir;
    inline static std::string s_current = "default";
    inline static Report s_lastReport;

    static constexpr int kFormatVersion = 2;

    // Everything after the first '=' is the value, so a value may
    // contain one. Keys may not, which is fine: we generate them.
    static bool SplitLine(const std::string& line,
                          std::string& key, std::string& value)
    {
        if (line.empty() || line[0] == '#') return false;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) return false;

        key = line.substr(0, eq);
        value = line.substr(eq + 1);

        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) { s.clear(); return; }
            s = s.substr(a, b - a + 1);
        };
        trim(key);
        trim(value);
        return !key.empty();
    }

    static bool ParseFloat(const std::string& s, float* out) {
        if (s.empty()) return false;
        char* end = nullptr;
        float v = std::strtof(s.c_str(), &end);
        if (!end || end == s.c_str()) return false;
        *out = v;
        return true;
    }

    static float Clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // A module name can contain spaces ("Auto Blockhit"), and so can
    // a setting name. The prefix tells us where the module name ends
    // because the separator we search for is the LAST known one.
    //
    //   module.Auto Blockhit.set.Block Range
    //           ^^^^^^^^^^^^     ^^^^^^^^^^^
    static bool SplitModuleKey(const std::string& key,
                               std::string& module,
                               std::string& field,
                               std::string& setting)
    {
        const std::string prefix = "module.";
        if (key.compare(0, prefix.size(), prefix) != 0) return false;

        std::string rest = key.substr(prefix.size());

        size_t marker = rest.rfind(".set.");
        if (marker != std::string::npos) {
            module  = rest.substr(0, marker);
            field   = "set";
            setting = rest.substr(marker + 5);
            return !module.empty() && !setting.empty();
        }

        size_t dot = rest.rfind('.');
        if (dot == std::string::npos) return false;
        module = rest.substr(0, dot);
        field  = rest.substr(dot + 1);
        setting.clear();
        return !module.empty() && !field.empty();
    }

public:
    // -------------------------------------------------------------
    // %APPDATA%\Phantom, created if missing.
    // -------------------------------------------------------------
    static const std::string& Directory() {
        if (!s_dir.empty()) return s_dir;

        char path[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
            s_dir = std::string(path) + "\\Phantom";
        } else {
            // No APPDATA is close to impossible, but a client that
            // silently stops saving is worse than one that falls
            // back to the working directory.
            s_dir = ".\\Phantom";
        }
        CreateDirectoryA(s_dir.c_str(), nullptr);
        return s_dir;
    }

    static std::string PathFor(const std::string& name) {
        return Directory() + "\\" + name + ".cfg";
    }

    static const std::string& Current() { return s_current; }
    static const Report& LastReport()   { return s_lastReport; }

    // -------------------------------------------------------------
    // Names of every .cfg in the folder.
    // -------------------------------------------------------------
    static std::vector<std::string> List() {
        std::vector<std::string> out;

        std::string pattern = Directory() + "\\*.cfg";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string n = fd.cFileName;
            if (n.size() > 4) n = n.substr(0, n.size() - 4);   // drop .cfg
            if (!n.empty()) out.push_back(n);
        } while (FindNextFileA(h, &fd));

        FindClose(h);
        std::sort(out.begin(), out.end());
        return out;
    }

    // -------------------------------------------------------------
    // Save
    // -------------------------------------------------------------
    // Written to a temp file and moved into place. A crash halfway
    // through a write would otherwise leave a truncated config that
    // loads as garbage, and the one moment a client is most likely
    // to crash is while it is being ejected.
    // -------------------------------------------------------------
    static bool Save(const std::string& name) {
        std::string finalPath = PathFor(name);
        std::string tempPath  = finalPath + ".tmp";

        {
            std::ofstream f(tempPath, std::ios::out | std::ios::trunc);
            if (!f.is_open()) {
                s_lastReport = Report{};
                s_lastReport.message = "Could not write " + name;
                return false;
            }

            f << "# Phantom config v" << kFormatVersion << "\n";
            f << "# Unknown keys are ignored, missing keys keep defaults.\n";
            f << "version=" << kFormatVersion << "\n\n";

            // ---- Interface ----
            using iOS::UI;
            f << "# Interface\n";
            f << "ui.scale="      << UI::scale      << "\n";
            f << "ui.animSpeed="  << UI::animSpeed  << "\n";
            f << "ui.roundness="  << UI::roundness  << "\n";
            f << "ui.glow="       << (UI::glow ? 1 : 0) << "\n";
            f << "ui.glowAmount=" << UI::glowAmount << "\n";
            f << "ui.dim="        << (UI::dim ? 1 : 0) << "\n";
            f << "ui.dimAmount="  << UI::dimAmount  << "\n";
            f << "ui.vignette="   << (UI::vignette ? 1 : 0) << "\n";
            f << "ui.openAnim="   << (UI::openAnimation ? 1 : 0) << "\n";
            f << "ui.hoverInfo="  << (UI::hoverInfo ? 1 : 0) << "\n";
            f << "ui.rowNudge="   << (UI::rowNudge ? 1 : 0) << "\n";
            f << "ui.accent="     << UI::accent << "\n";
            for (int i = 0; i < 4; i++)
                f << "ui.accentCustom" << i << "=" << UI::accentCustom[i] << "\n";
            f << "\n";

            // ---- HUD ----
            using iOS::HUD;
            f << "# HUD\n";
            f << "hud.watermark="  << (HUD::watermark ? 1 : 0) << "\n";
            f << "hud.moduleList=" << (HUD::moduleList ? 1 : 0) << "\n";
            f << "hud.fps="        << (HUD::showFps ? 1 : 0) << "\n";
            f << "hud.wmX="        << HUD::wmX << "\n";
            f << "hud.wmY="        << HUD::wmY << "\n";
            f << "hud.mlX="        << HUD::mlX << "\n";
            f << "hud.mlY="        << HUD::mlY << "\n";
            f << "\n";

            // ---- Modules ----
            for (auto& m : ModuleManager::GetModules()) {
                const std::string& n = m->GetName();
                f << "# " << n << "\n";
                f << "module." << n << ".enabled=" << (m->IsEnabled() ? 1 : 0) << "\n";
                f << "module." << n << ".key=" << m->GetKeybind() << "\n";

                // Setting::Read gives the stored value for whatever
                // the type is. Writing this by hand is how a mode,
                // which is an int, was being printed as a float read
                // off an int pointer: garbage in the file and a
                // random mode on load.
                //
                // A colour is the one setting with more than one
                // number, so it is written as four suffixed keys the
                // way ui.accentCustom is. Everything else is a single
                // key, byte-for-byte as before.
                for (const auto& s : m->GetSettings()) {
                    if (s.type == Setting::Type::Color) {
                        static const char* comp[4] = { "r", "g", "b", "a" };
                        for (int i = 0; i < 4; i++)
                            f << "module." << n << ".set." << s.name << "."
                              << comp[i] << "=" << s.ReadComp(i) << "\n";
                    } else {
                        f << "module." << n << ".set." << s.name << "="
                          << s.Read() << "\n";
                    }
                }

                f << "\n";
            }
        }

        // MoveFileEx with REPLACE_EXISTING is the atomic swap
        if (!MoveFileExA(tempPath.c_str(), finalPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(tempPath.c_str());
            s_lastReport = Report{};
            s_lastReport.message = "Could not replace " + name;
            return false;
        }

        s_current = name;
        s_lastReport = Report{};
        s_lastReport.ok = true;
        s_lastReport.message = "Saved " + name;
        return true;
    }

    // -------------------------------------------------------------
    // Load
    // -------------------------------------------------------------
    // Must run on the CLIENT thread: enabling a module calls
    // OnEnable, which needs a live JNIEnv.
    // -------------------------------------------------------------
    static Report Load(const std::string& name, JNIEnv* env) {
        Report r;

        std::ifstream f(PathFor(name));
        if (!f.is_open()) {
            r.message = "No config named " + name;
            s_lastReport = r;
            return r;
        }

        // Enables are deferred to the end. A module's OnEnable may
        // read its settings, so every value has to be in place
        // before anything is switched on.
        std::vector<std::pair<Module*, bool>> toggles;

        std::string line, key, value;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;

            if (!SplitLine(line, key, value)) { r.malformed++; continue; }

            float v = 0.0f;

            // ---- Interface ----
            if (key.compare(0, 3, "ui.") == 0) {
                using iOS::UI;
                if (!ParseFloat(value, &v)) { r.malformed++; continue; }

                std::string k = key.substr(3);
                if      (k == "scale")      UI::scale = v;
                else if (k == "animSpeed")  UI::animSpeed = v;
                else if (k == "roundness")  UI::roundness = v;
                else if (k == "glow")       UI::glow = (v != 0.0f);
                else if (k == "glowAmount") UI::glowAmount = v;
                else if (k == "dim")        UI::dim = (v != 0.0f);
                else if (k == "dimAmount")  UI::dimAmount = v;
                else if (k == "vignette")   UI::vignette = (v != 0.0f);
                else if (k == "openAnim")   UI::openAnimation = (v != 0.0f);
                else if (k == "hoverInfo")  UI::hoverInfo = (v != 0.0f);
                else if (k == "rowNudge")   UI::rowNudge = (v != 0.0f);
                else if (k == "accent")     UI::accent = (int)v;
                else if (k.compare(0, 12, "accentCustom") == 0 && k.size() == 13) {
                    int idx = k[12] - '0';
                    if (idx >= 0 && idx < 4) UI::accentCustom[idx] = v;
                    else { r.unknown++; continue; }
                }
                else { r.unknown++; continue; }

                r.applied++;
                continue;
            }

            // ---- HUD ----
            if (key.compare(0, 4, "hud.") == 0) {
                using iOS::HUD;
                if (!ParseFloat(value, &v)) { r.malformed++; continue; }

                std::string k = key.substr(4);
                if      (k == "watermark")  HUD::watermark = (v != 0.0f);
                else if (k == "moduleList") HUD::moduleList = (v != 0.0f);
                else if (k == "fps")        HUD::showFps = (v != 0.0f);
                else if (k == "wmX")        HUD::wmX = Clamp01(v);
                else if (k == "wmY")        HUD::wmY = Clamp01(v);
                else if (k == "mlX")        HUD::mlX = Clamp01(v);
                else if (k == "mlY")        HUD::mlY = Clamp01(v);
                else if (k == "corner") {
                    // v1 config. Both elements shared one corner, so
                    // the old look is reproduced by putting them in
                    // the same place and letting the module list sit
                    // under the watermark.
                    int c = (int)v;
                    bool right  = (c == 1 || c == 3);
                    bool bottom = (c == 2 || c == 3);
                    HUD::wmX = HUD::mlX = right  ? 0.988f : 0.012f;
                    HUD::wmY = HUD::mlY = bottom ? 0.955f : 0.020f;
                    r.migrated++;
                }
                else { r.unknown++; continue; }

                r.applied++;
                continue;
            }

            if (key == "version") continue;   // informational

            // ---- Modules ----
            std::string modName, field, setting;
            if (!SplitModuleKey(key, modName, field, setting)) {
                r.unknown++;
                continue;
            }

            // A config from a build that had a module this one does
            // not. Skipped, never fatal.
            Module* mod = ModuleManager::Find(modName);
            if (!mod) { r.unknown++; continue; }

            if (!ParseFloat(value, &v)) { r.malformed++; continue; }

            if (field == "enabled") {
                toggles.push_back({ mod, v != 0.0f });
                r.applied++;
            } else if (field == "key") {
                int k = (int)v;
                // A key outside the virtual-key range would never
                // fire and would show as nonsense on the chip.
                mod->SetKeybind((k > 0 && k < 256) ? k : 0);
                r.applied++;
            } else if (field == "set") {
                // A colour persists as <name>.r/.g/.b/.a. Detect that
                // suffix and route to the channel, but only when the
                // base name is really a colour, so a scalar setting
                // that happens to end in .r is left untouched and
                // still handled by SetValue below.
                int comp = -1;
                std::string base = setting;
                if (setting.size() > 2 && setting[setting.size() - 2] == '.') {
                    char ch = setting.back();
                    if      (ch == 'r') comp = 0;
                    else if (ch == 'g') comp = 1;
                    else if (ch == 'b') comp = 2;
                    else if (ch == 'a') comp = 3;
                    if (comp >= 0) base = setting.substr(0, setting.size() - 2);
                }

                bool ok = (comp >= 0) && mod->SetColorComponent(base, comp, v);
                if (!ok) ok = mod->SetValue(setting, v);

                // SetValue clamps to the setting's own range, so a
                // hand-edited or outdated file cannot push a module
                // somewhere its interface could never reach.
                if (ok) r.applied++;
                else    r.unknown++;   // a setting that has been renamed
            } else {
                r.unknown++;
            }
        }

        // Clamp anything the file could have put out of range. A
        // hand-edited config should never be able to leave the menu
        // unusably large or invisible.
        iOS::UI::Apply();

        for (auto& t : toggles) {
            try {
                t.first->SetEnabled(t.second, env);
            } catch (...) {
                // A module that throws while being switched on must
                // not abort the whole load.
            }
            if (env && env->ExceptionCheck()) env->ExceptionClear();
        }

        s_current = name;
        r.ok = true;

        char buf[160];
        if (r.unknown || r.malformed) {
            snprintf(buf, sizeof(buf), "%d values restored, %d skipped",
                     r.applied, r.unknown + r.malformed);
        } else if (r.migrated) {
            snprintf(buf, sizeof(buf), "%d values restored, layout updated",
                     r.applied);
        } else {
            snprintf(buf, sizeof(buf), "%d values restored", r.applied);
        }
        r.message = buf;

        s_lastReport = r;
        return r;
    }

    static bool Exists(const std::string& name) {
        DWORD a = GetFileAttributesA(PathFor(name).c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    static bool Delete(const std::string& name) {
        if (!DeleteFileA(PathFor(name).c_str())) {
            s_lastReport = Report{};
            s_lastReport.message = "Could not delete " + name;
            return false;
        }
        // Deleting the config you are running would leave the panel
        // pointing at a file that no longer exists.
        if (s_current == name) s_current = "default";
        s_lastReport = Report{};
        s_lastReport.ok = true;
        s_lastReport.message = "Deleted " + name;
        return true;
    }

    // Sanitise a name typed by the user into something the file
    // system will accept, rather than failing on a stray slash.
    static std::string SanitiseName(const char* raw) {
        std::string out;
        for (const char* c = raw; *c && out.size() < 32; ++c) {
            char ch = *c;
            bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                   || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'
                   || ch == ' ';
            if (ok) out += ch;
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }
};
