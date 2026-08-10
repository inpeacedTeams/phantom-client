#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#include "ios_theme.h"
#include "ios_widgets.h"
#include "ios_hud.h"
#include "notifications.h"
#include "../config/config_store.h"
#include "../config/profiles.h"
#include "../modules/module_manager.h"

// =================================================================
// Config panel
// =================================================================
// Two different things share this screen and it is worth being
// clear about the difference, because conflating them is confusing:
//
//   PRESETS are built in. They are opinionated starting points for
//   a particular anticheat, and applying one overwrites what you
//   have. They cannot be changed or deleted.
//
//   CONFIGS are yours. Saved to disk, loaded by name, deleted when
//   you are done with them. "default" is written automatically on
//   eject and once a minute, so doing nothing at all still gets you
//   persistence.
//
// Lives in its own file because Menu was becoming the sort of class
// that does everything, and this part has no business knowing how a
// module row is drawn.
//
// THREADING
// Render thread. Loading touches modules and therefore JNI, so it
// is queued onto the client thread rather than done here. Saving
// only READS module state, which is safe from either side.
// =================================================================

namespace iOS {

class ConfigPanel {
private:
    inline static char s_newName[40] = "";
    inline static std::string s_confirmDelete;
    inline static std::vector<std::string> s_cache;
    inline static float s_refreshIn = 0.0f;

    // Presets are compile-time constants dressed up as function
    // calls. Profiles::All() builds four structs, each holding
    // three vectors and up to forty entries, and it was being
    // called once per frame: several hundred allocations a second
    // to draw a list that never changes.
    static const std::vector<Profile>& Presets() {
        static const std::vector<Profile> presets = Profiles::All();
        return presets;
    }

    // Listing a directory every frame is a syscall per frame for
    // data that changes when the user saves something. Once a second
    // is plenty, and it refreshes immediately after any write.
    static void RefreshList(bool force = false) {
        s_refreshIn -= ImGui::GetIO().DeltaTime;
        if (force || s_refreshIn <= 0.0f) {
            s_cache = ConfigStore::List();
            s_refreshIn = 1.0f;
        }
    }

    static void QueueLoad(const std::string& name) {
        ModuleManager::QueueAction([name](JNIEnv* env) {
            ConfigStore::Report r = ConfigStore::Load(name, env);
            if (r.ok) Notify::Success("Loaded " + name, r.message);
            else      Notify::Error("Could not load " + name, r.message);
        });
    }

    // ---------------------------------------------------------
    // One saved config: name, and the actions for it
    // ---------------------------------------------------------
    static void ConfigRow(const std::string& name, bool last) {
        ImGui::PushID(name.c_str());

        bool isCurrent = (name == ConfigStore::Current());
        bool confirming = (s_confirmDelete == name);

        float w = ImGui::GetContentRegionAvail().x;
        float h = M::RowHeight;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cy = p.y + h * 0.5f;

        // ---- Buttons on the right, measured first ----
        float btnH = 24.0f * UI::scale;
        float pad  = 8.0f * UI::scale;

        Fonts::Push(Fonts::Caption);
        const char* loadTxt = "Load";
        const char* delTxt  = confirming ? "Sure?" : "Delete";
        ImVec2 loadSz = ImGui::CalcTextSize(loadTxt);
        ImVec2 delSz  = ImGui::CalcTextSize(delTxt);
        Fonts::Pop(Fonts::Caption);

        float loadW = loadSz.x + 22.0f * UI::scale;
        float delW  = delSz.x + 22.0f * UI::scale;
        float right = p.x + w - M::RowPadX;

        // ---- Delete ----
        ImVec2 da(right - delW, cy - btnH * 0.5f);
        ImGui::SetCursorScreenPos(da);
        ImGui::InvisibleButton("##del", ImVec2(delW, btnH));
        bool delHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            if (confirming) {
                if (ConfigStore::Delete(name)) {
                    Notify::Info("Deleted " + name);
                    RefreshList(true);
                } else {
                    Notify::Error("Could not delete " + name,
                                  ConfigStore::LastReport().message);
                }
                s_confirmDelete.clear();
            } else {
                // Two-step, because there is no undo for a file.
                s_confirmDelete = name;
            }
        }

        float delHov = Anim::To(ImGui::GetID("##delh"),
                                delHover ? 1.0f : 0.0f, 18.0f);
        ImU32 delTint = confirming ? Col::Red : Col::Label2;
        dl->AddRectFilled(da, ImVec2(da.x + delW, da.y + btnH),
                          Col::Alpha(delTint, 0.10f + 0.14f * delHov),
                          7.0f * UI::roundness + 1.0f);
        Fonts::Push(Fonts::Caption);
        dl->AddText(ImVec2(da.x + (delW - delSz.x) * 0.5f, cy - delSz.y * 0.5f),
                    delTint, delTxt);
        Fonts::Pop(Fonts::Caption);

        // ---- Load ----
        ImVec2 la(da.x - pad - loadW, cy - btnH * 0.5f);
        ImGui::SetCursorScreenPos(la);
        ImGui::InvisibleButton("##load", ImVec2(loadW, btnH));
        bool loadHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            QueueLoad(name);
            s_confirmDelete.clear();
        }

        float loadHov = Anim::To(ImGui::GetID("##loadh"),
                                 loadHover ? 1.0f : 0.0f, 18.0f);
        ImVec2 lb(la.x + loadW, la.y + btnH);
        if (loadHov > 0.02f)
            Glow(dl, la, lb, Col::Blue, 7.0f, loadHov * 0.5f, 3);
        dl->AddRectFilled(la, lb,
                          Col::Alpha(Col::Blue, 0.12f + 0.18f * loadHov),
                          7.0f * UI::roundness + 1.0f);
        Fonts::Push(Fonts::Caption);
        dl->AddText(ImVec2(la.x + (loadW - loadSz.x) * 0.5f, cy - loadSz.y * 0.5f),
                    Col::Blue, loadTxt);
        Fonts::Pop(Fonts::Caption);

        // ---- Name ----
        // Truncated with an ellipsis rather than allowed to run into
        // the buttons, which is what a long name used to do.
        float nameRoom = (la.x - pad) - (p.x + M::RowPadX);
        std::string shown = name;
        ImVec2 ns = ImGui::CalcTextSize(shown.c_str());
        if (ns.x > nameRoom && nameRoom > 24.0f) {
            while (shown.size() > 1 && ns.x > nameRoom - 12.0f) {
                shown.pop_back();
                ns = ImGui::CalcTextSize(shown.c_str());
            }
            shown += "...";
            ns = ImGui::CalcTextSize(shown.c_str());
        }

        dl->AddText(ImVec2(p.x + M::RowPadX, cy - ns.y * 0.5f),
                    Col::Label, shown.c_str());

        // A dot marks the one currently loaded, so "which of these
        // am I actually running" is answerable at a glance.
        if (isCurrent) {
            float dx = p.x + M::RowPadX + ns.x + 9.0f;
            dl->AddCircleFilled(ImVec2(dx, cy), 3.5f, Col::Green, 12);
        }

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
        if (!last) RowSeparator();

        ImGui::PopID();
    }

public:
    static void Render() {
        RefreshList();

        // =====================================================
        // Your configs
        // =====================================================
        SectionHeader("Your configs");

        BeginCard();
        if (s_cache.empty()) {
            ImGui::Dummy(ImVec2(0, 14));
            ImGui::Indent(M::RowPadX);
            Fonts::Push(Fonts::Caption);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(Col::Label3));
            ImGui::TextUnformatted("Nothing saved yet.");
            ImGui::PopStyleColor();
            Fonts::Pop(Fonts::Caption);
            ImGui::Unindent(M::RowPadX);
            ImGui::Dummy(ImVec2(0, 14));
        } else {
            for (size_t i = 0; i < s_cache.size(); i++)
                ConfigRow(s_cache[i], i + 1 == s_cache.size());
        }
        EndCard();

        // ---- Save ----
        BeginCard();
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Indent(M::RowPadX);

        float avail = ImGui::GetContentRegionAvail().x - M::RowPadX;
        float btnW  = 92.0f * UI::scale;

        ImGui::PushItemWidth(avail - btnW - 10.0f);
        ImGui::InputTextWithHint("##name", "Name this config",
                                 s_newName, sizeof(s_newName));
        ImGui::PopItemWidth();

        ImGui::SameLine(0, 10.0f);

        std::string clean = ConfigStore::SanitiseName(s_newName);
        bool canSave = !clean.empty();
        bool overwrite = canSave && ConfigStore::Exists(clean);

        if (!canSave) ImGui::BeginDisabled();
        if (Button(overwrite ? "Replace" : "Save", btnW)) {
            if (ConfigStore::Save(clean)) {
                Notify::Success("Saved " + clean,
                                "Load it any time from this screen.");
                s_newName[0] = '\0';
                RefreshList(true);
            } else {
                Notify::Error("Could not save",
                              ConfigStore::LastReport().message);
            }
        }
        if (!canSave) ImGui::EndDisabled();

        if (overwrite) {
            Fonts::Push(Fonts::Caption);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(Col::Orange));
            ImGui::TextUnformatted("A config with that name already exists.");
            ImGui::PopStyleColor();
            Fonts::Pop(Fonts::Caption);
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Unindent(M::RowPadX);
        EndCard();

        Footnote("Everything is saved automatically to \"default\" when you "
                 "eject and once a minute while you play, so a crash never "
                 "costs you a session. Named configs are for keeping several "
                 "setups side by side.");

        // =====================================================
        // Presets
        // =====================================================
        SectionHeader("Presets");

        const std::vector<Profile>& presets = Presets();
        for (size_t i = 0; i < presets.size(); i++) {
            ImGui::PushID((int)(1000 + i));

            BeginCard();
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Indent(M::RowPadX);

            Fonts::Push(Fonts::BodyBold);
            ImGui::TextUnformatted(presets[i].name);
            Fonts::Pop(Fonts::BodyBold);

            Fonts::Push(Fonts::Caption);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(Col::Label2));
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 8.0f);
            ImGui::TextUnformatted(presets[i].description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            Fonts::Pop(Fonts::Caption);

            ImGui::Dummy(ImVec2(0, 8));

            if (Button("Apply", ImGui::GetContentRegionAvail().x - M::RowPadX,
                       Col::Blue, false)) {
                // Copied into the lambda: the action runs on the
                // client thread a tick later, and a reference into a
                // static is fine but a copy is one less thing to
                // reason about.
                Profile p = presets[i];
                std::string label = p.name;
                ModuleManager::QueueAction([p, label](JNIEnv* env) {
                    Profiles::Apply(p, env);
                    if (Profiles::LastReport().find("could not") ==
                        std::string::npos) {
                        Notify::Success(label + " applied",
                                        Profiles::LastReport());
                    }
                    // The failure case already raised its own
                    // warning inside Apply, with the names in it.
                });
            }

            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Unindent(M::RowPadX);
            EndCard();

            ImGui::PopID();
        }

        Footnote("A preset overwrites your current settings. Save what you "
                 "have first if you want it back.");

        // =====================================================
        // HUD, then the boring facts
        // =====================================================
        ImGui::Dummy(ImVec2(0, 8));
        HUD::RenderSettings();

        SectionHeader("About");
        BeginCard();
        ValueRow("Version", "3.0.0");
        RowSeparator();
        ValueRow("Target", "Lunar Client 1.8.9");
        RowSeparator();
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", ModuleManager::GetModuleCount());
            ValueRow("Modules", buf);
        }
        RowSeparator();
        ValueRow("Config", ConfigStore::Current().c_str());
        EndCard();

        Footnote("Configs live in %APPDATA%\\Phantom. They are plain text, "
                 "so you can open one and read it.");

        ImGui::Dummy(ImVec2(0, 10));
    }

    // Called when the menu closes, so a half-typed name and a
    // pending delete confirmation do not survive until next time.
    static void Reset() {
        s_confirmDelete.clear();
        s_refreshIn = 0.0f;
    }
};

} // namespace iOS
