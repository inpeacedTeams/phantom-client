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
// EDGE DETECTION
// The old test looked at the fractional part of the player's
// coordinates and sneaked whenever it was near a block boundary.
// Walking across a flat floor crosses a boundary every block, so it
// crouched constantly on solid ground: slower than not having it,
// and nothing to do with bridging. The real question is whether
// there is AIR under the place you are about to be, so this
// projects your movement forward and asks the world.
//
// WHY THE SNEAK STATE IS RE-APPLIED EVERY TICK
// It used to skip the write whenever its own cached flag already
// matched. That assumes nothing else touches the sneak key, and
// something does: Sprint Reset in Sneak Tap mode drives the same
// bind, and when it hands the key back it restores the hardware
// state, which is "shift is not held". Bridge Assist still believed
// it was sneaking, never wrote again, and you walked off the edge.
//
// Writing a boolean that is already true costs nothing, so the
// state is simply re-asserted.
//
// MODES
//   0 Eagle     sneak at the edge, release once clear
//   1 Godbridge rapid toggling, faster but noisier movement
//   2 Breezily  single-tick sneak, fastest and least forgiving
//   3 Safewalk  never step off an edge, any direction
// =================================================================

class BridgeAssist : public Module {
private:
    // ---- Core ----
    int   m_mode         = 0;
    bool  m_onlyBackward = true;

    // ---- Advanced ----
    bool  m_onlyOnGround = true;
    float m_lookAhead    = 0.34f;  // roughly one tick at sprint speed
    bool  m_useWorld     = true;   // real block test when available
    float m_edgeFallback = 0.28f;  // fractional test if it is not
    int   m_sneakTicks   = 2;
    int   m_unsneakTicks = 1;
    int   m_holdTicks    = 2;      // stay down briefly after clearing

    // ---- State ----
    bool m_sneaking     = false;   // what we WANT, not what we assume
    int  m_sneakCounter = 0;
    int  m_holdLeft     = 0;
    bool m_atEdge       = false;

    // Readout
    const char* m_why = "idle";
    bool m_worldOk = false;

    // Re-asserted every tick rather than only on a change, because
    // another module may have handed the key back since our last
    // write and we would never notice.
    void ApplySneak(JNIEnv* env, bool on) {
        if (on) {
            KeyBinds::SetSneak(env, true);
        } else if (m_sneaking) {
            KeyBinds::ReleaseSneak(env);
        }
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

        // Losing the player mid-sneak used to leave shift held all
        // the way into the next world.
        if (!player) {
            ApplySneak(env, false);
            m_sneakCounter = 0;
            m_holdLeft = 0;
            m_why = "no player";
            return;
        }

        if (Minecraft::IsInGui(env)) {
            ApplySneak(env, false);
            m_why = "menu";
            return;
        }

        // Sneaking mid-air does nothing except look strange
        if (m_onlyOnGround && !Minecraft::IsOnGround(env, player)) {
            ApplySneak(env, false);
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
            ApplySneak(env, false);
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
                    ApplySneak(env, false);
                    m_sneakCounter = 0;
                    m_why = "ground ahead";
                    break;
                }
                m_sneakCounter++;
                int limit = m_sneaking ? m_sneakTicks : m_unsneakTicks;
                if (m_sneakCounter >= limit) {
                    ApplySneak(env, !m_sneaking);
                    m_sneakCounter = 0;
                } else {
                    ApplySneak(env, m_sneaking);   // hold the current state
                }
                m_why = "godbridge";
                break;
            }
            case 2: {   // Breezily: one tick down, one tick up
                if (want) { ApplySneak(env, !m_sneaking); m_why = "breezily"; }
                else      { ApplySneak(env, false); m_why = "ground ahead"; }
                break;
            }
            default: {  // Eagle and Safewalk
                ApplySneak(env, want);
                m_why = want ? "edge ahead" : "ground ahead";
                break;
            }
        }
    }

    void OnDisable(JNIEnv* env) override {
        ApplySneak(env, false);
        m_sneakCounter = 0;
        m_holdLeft = 0;
        m_why = "off";
    }

    const char* StatusLine() const override {
        return m_sneaking ? "sneaking" : nullptr;
    }

    bool HasAdvanced() const override { return true; }

    void RenderSettings() override {
        const char* modes[] = { "Eagle", "Godbridge", "Breezily", "Safewalk" };
        ImGui::Combo("Mode", &m_mode, modes, 4);
        switch (m_mode) {
            case 0: ImGui::TextDisabled("Sneak at the edge, release once clear."); break;
            case 1: ImGui::TextDisabled("Rapid toggling. Faster, rougher movement."); break;
            case 2: ImGui::TextDisabled("Single-tick sneak. Fastest, least forgiving."); break;
            case 3: ImGui::TextDisabled("Never step off an edge, in any direction."); break;
        }

        if (m_mode != 3)
            ImGui::Checkbox("Only While Walking Backwards", &m_onlyBackward);

        if (m_useWorld && !m_worldOk) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "World unresolved: falling back to a coarse edge guess");
        }
        if (!KeyBinds::HasSneak()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Sneak keybind unresolved: module inactive");
        }
    }

    void RenderAdvanced() override {
        ImGui::TextDisabled("%s  ·  %s", m_sneaking ? "SNEAKING" : "upright", m_why);

        ImGui::SeparatorText("Detection");
        ImGui::Checkbox("Use Block Lookup", &m_useWorld);
        if (m_useWorld) {
            ImGui::TextDisabled(m_worldOk
                ? "World readable: real edge detection"
                : "World unresolved: using the fractional guess");
        }
        ImGui::SliderFloat("Look Ahead", &m_lookAhead, 0.1f, 0.7f, "%.2f blocks");
        ImGui::TextDisabled("Roughly one tick of movement at sprint speed.");
        if (!m_useWorld || !m_worldOk)
            ImGui::SliderFloat("Edge Fallback", &m_edgeFallback, 0.05f, 0.49f, "%.2f");

        ImGui::SeparatorText("Timing");
        ImGui::SliderInt("Hold Ticks", &m_holdTicks, 0, 6);
        ImGui::TextDisabled("Stays down briefly after the ground returns.");
        if (m_mode == 1) {
            ImGui::SliderInt("Sneak Ticks", &m_sneakTicks, 1, 5);
            ImGui::SliderInt("Unsneak Ticks", &m_unsneakTicks, 1, 3);
        }

        ImGui::Checkbox("Only On Ground", &m_onlyOnGround);
    }
};
