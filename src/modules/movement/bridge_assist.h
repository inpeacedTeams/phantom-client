#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/world.h"
#include <imgui.h>
#include <Windows.h>
#include <cmath>

// =================================================================
// Bridge Assist (AutoEagle)
// =================================================================
// Holds sneak when the next step would take you off the block, so
// you can bridge backwards without looking down.
//
// WHAT WAS BROKEN
// The old edge test looked at the fractional part of the player's
// coordinates and sneaked whenever it was near a boundary. Walking
// across a flat floor crosses a boundary every block, so it
// crouched constantly on solid ground: slower than not having it,
// and nothing to do with bridging.
//
// The real question is whether there is AIR under the place you are
// about to be. This version projects your movement forward and asks
// the world.
//
// WHY SNEAK RATHER THAN ANYTHING CLEVERER
// Holding shift is a vanilla input. The packet stream is a plain
// sneak toggle, which is what a careful bridger produces anyway, so
// there is nothing here for a server to flag. Driving the keybind
// also matters mechanically: setSneaking() is recomputed from the
// key every tick and would be overwritten.
//
// MODES
//   0 Eagle     sneak at the edge, release once clear
//   1 Godbridge rapid toggling, faster but noisier movement
//   2 Breezily  single-tick sneak, fastest and least forgiving
//   3 Safewalk  never step off an edge, any direction
// =================================================================

class BridgeAssist : public Module {
private:
    int   m_mode         = 0;
    bool  m_onlyBackward = true;
    bool  m_onlyOnGround = true;

    // How far ahead to look, in blocks. Roughly one tick of
    // movement at sprint speed.
    float m_lookAhead    = 0.34f;
    bool  m_useWorld     = true;   // real block test when available
    float m_edgeFallback = 0.28f;  // fractional test if it is not

    int   m_sneakTicks   = 2;
    int   m_unsneakTicks = 1;
    int   m_holdTicks    = 2;      // stay down briefly after clearing

    // ---- State ----
    bool m_sneaking     = false;
    int  m_sneakCounter = 0;
    int  m_holdLeft     = 0;
    bool m_atEdge       = false;

    // Readout
    const char* m_why = "idle";
    bool m_worldOk = false;

    void SetSneak(JNIEnv* env, bool on) {
        if (m_sneaking == on) return;
        if (on) KeyBinds::SetSneak(env, true);
        else    KeyBinds::ReleaseSneak(env);
        m_sneaking = on;
    }

    // Where will we be shortly, given the direction being held?
    // Uses the movement keys rather than the motion vector, because
    // motion is already zero on the tick you would step off.
    void ProjectAhead(JNIEnv* env, jobject player,
                      double* outX, double* outZ)
    {
        double x = Minecraft::GetPosX(env, player);
        double z = Minecraft::GetPosZ(env, player);

        float fwd = (KeyBinds::GetForward(env) ? 1.f : 0.f)
                  - (KeyBinds::GetBack(env)    ? 1.f : 0.f);
        float str = (KeyBinds::GetLeft(env)    ? 1.f : 0.f)
                  - (KeyBinds::GetRight(env)   ? 1.f : 0.f);

        if (fwd == 0.f && str == 0.f) { *outX = x; *outZ = z; return; }

        const double DEG = 3.14159265358979 / 180.0;
        double yaw = Minecraft::GetYaw(env, player) * DEG;

        // Forward and right in world space
        double fx = -std::sin(yaw), fz = std::cos(yaw);
        double rx =  std::cos(yaw), rz = std::sin(yaw);

        double dx = fx * fwd - rx * str;
        double dz = fz * fwd - rz * str;

        double len = std::sqrt(dx * dx + dz * dz);
        if (len > 0.001) { dx /= len; dz /= len; }

        *outX = x + dx * m_lookAhead;
        *outZ = z + dz * m_lookAhead;
    }

    // Is the ground about to run out?
    bool CheckEdge(JNIEnv* env, jobject player) {
        double ax, az;
        ProjectAhead(env, player, &ax, &az);

        double y = Minecraft::GetPosY(env, player);

        if (m_useWorld && m_worldOk) {
            // The honest test: is there anything to stand on there?
            return !World::GroundBelow(env, ax, y, az);
        }

        // Fallback when the block lookup could not be resolved.
        // Crude, but only reached when the real test is unavailable.
        double fx = ax - std::floor(ax);
        double fz = az - std::floor(az);
        double d = (double)m_edgeFallback;
        return (fx < d) || (fx > 1.0 - d) || (fz < d) || (fz > 1.0 - d);
    }

public:
    BridgeAssist()
        : Module("Bridge Assist", "Sneak when the next step would drop you",
                 ModuleCategory::MOVEMENT, 0)
    {
        Bind("Mode", &m_mode);
        Bind("Only Backward", &m_onlyBackward);
        Bind("Only On Ground", &m_onlyOnGround);
        Bind("Look Ahead", &m_lookAhead);
        Bind("Use World", &m_useWorld);
        Bind("Edge Fallback", &m_edgeFallback);
        Bind("Sneak Ticks", &m_sneakTicks);
        Bind("Unsneak Ticks", &m_unsneakTicks);
        Bind("Hold Ticks", &m_holdTicks);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;
        m_worldOk = World::Init(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (Minecraft::IsInGui(env)) {
            SetSneak(env, false);
            m_why = "menu";
            return;
        }

        // Sneaking mid-air does nothing except look strange
        if (m_onlyOnGround && !Minecraft::IsOnGround(env, player)) {
            SetSneak(env, false);
            m_sneakCounter = 0;
            m_holdLeft = 0;
            m_why = "airborne";
            return;
        }

        bool back = KeyBinds::GetBack(env);
        bool any  = back || KeyBinds::GetForward(env)
                 || KeyBinds::GetLeft(env) || KeyBinds::GetRight(env);

        bool active = (m_mode == 3) ? any                     // Safewalk
                    : (m_onlyBackward ? back : any);

        if (!active) {
            SetSneak(env, false);
            m_sneakCounter = 0;
            m_holdLeft = 0;
            m_why = "not moving";
            return;
        }

        m_atEdge = CheckEdge(env, player);

        // Releasing the instant the ground reappears means the very
        // next tick can step off again, so hold on a moment.
        if (m_atEdge) m_holdLeft = m_holdTicks;
        else if (m_holdLeft > 0) m_holdLeft--;

        bool want = m_atEdge || m_holdLeft > 0;

        switch (m_mode) {
            case 1: {   // Godbridge: toggle while over the drop
                if (!want) {
                    SetSneak(env, false);
                    m_sneakCounter = 0;
                    m_why = "ground ahead";
                    break;
                }
                m_sneakCounter++;
                int limit = m_sneaking ? m_sneakTicks : m_unsneakTicks;
                if (m_sneakCounter >= limit) {
                    SetSneak(env, !m_sneaking);
                    m_sneakCounter = 0;
                }
                m_why = "godbridge";
                break;
            }
            case 2: {   // Breezily: one tick down, one tick up
                if (want) { SetSneak(env, !m_sneaking); m_why = "breezily"; }
                else      { SetSneak(env, false); m_why = "ground ahead"; }
                break;
            }
            default: {  // Eagle and Safewalk
                SetSneak(env, want);
                m_why = want ? "edge ahead" : "ground ahead";
                break;
            }
        }
    }

    void OnDisable(JNIEnv* env) override {
        SetSneak(env, false);
        m_sneakCounter = 0;
        m_holdLeft = 0;
    }

    void RenderSettings() override {
        ImGui::TextColored(m_sneaking ? ImVec4(0.2f, 0.8f, 0.4f, 1.f)
                                      : ImVec4(0.55f, 0.55f, 0.6f, 1.f),
            "%s  (%s)", m_sneaking ? "SNEAKING" : "upright", m_why);

        ImGui::Separator();
        const char* modes[] = { "Eagle", "Godbridge", "Breezily", "Safewalk" };
        ImGui::Combo("Mode", &m_mode, modes, 4);
        switch (m_mode) {
            case 0: ImGui::TextDisabled("Sneak at the edge, release once clear."); break;
            case 1: ImGui::TextDisabled("Rapid toggling. Faster, rougher movement."); break;
            case 2: ImGui::TextDisabled("Single-tick sneak. Fastest, least forgiving."); break;
            case 3: ImGui::TextDisabled("Never step off an edge, in any direction."); break;
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Detection");
        ImGui::Checkbox("Use Block Lookup", &m_useWorld);
        if (m_useWorld) {
            ImGui::TextColored(m_worldOk ? ImVec4(0.3f, 0.8f, 0.4f, 1.f)
                                         : ImVec4(1.f, 0.7f, 0.3f, 1.f),
                m_worldOk ? "World readable: real edge detection"
                          : "World unresolved: using the fractional guess");
        }
        ImGui::SliderFloat("Look Ahead", &m_lookAhead, 0.1f, 0.7f, "%.2f blocks");
        ImGui::TextDisabled("Roughly one tick of movement at sprint speed.");
        if (!m_useWorld || !m_worldOk)
            ImGui::SliderFloat("Edge Fallback", &m_edgeFallback, 0.05f, 0.49f, "%.2f");

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Timing");
        ImGui::SliderInt("Hold Ticks", &m_holdTicks, 0, 6);
        ImGui::TextDisabled("Stays down briefly after the ground returns.");
        if (m_mode == 1) {
            ImGui::SliderInt("Sneak Ticks", &m_sneakTicks, 1, 5);
            ImGui::SliderInt("Unsneak Ticks", &m_unsneakTicks, 1, 3);
        }

        ImGui::Separator();
        ImGui::Checkbox("Only Backward", &m_onlyBackward);
        ImGui::Checkbox("Only On Ground", &m_onlyOnGround);

        if (!KeyBinds::HasSneak()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Sneak keybind unresolved: module inactive");
        }
    }
};
