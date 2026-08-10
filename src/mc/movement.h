#pragma once
#include <jni.h>
#include <cmath>

#include "minecraft.h"
#include "keybinds.h"

// =================================================================
// Movement
// =================================================================
// The direction the player is asking to go.
//
// This was written out by hand in Speed, Fly and Bridge Assist, in
// three slightly different ways, and one of them had the diagonal
// signs the wrong way round. It is the same twelve lines every
// time, so here it is once.
//
// WHY THE KEYS AND NOT THE MOTION VECTOR
// Motion is the RESULT of last tick's movement. On the tick you
// walk into a wall, or the tick you would step off a ledge, it is
// already zero, which is exactly when a module most needs to know
// where you were trying to go.
//
// WHY THESE PARTICULAR ANGLES
// EntityLivingBase.moveEntityWithHeading resolves a diagonal by
// rotating the facing angle 45 degrees, not by normalising a
// vector. Reproducing the rotation rather than inventing our own
// keeps anything we compute identical to what the game will do.
// =================================================================

namespace Movement {

constexpr double kDegToRad = 3.14159265358979 / 180.0;

// Vanilla sprint speed, blocks per tick, on flat ground with no
// effects. Used as the reference any speed multiplier scales.
constexpr double kSprintSpeed = 0.2873;

struct Input {
    float forward = 0.0f;   // +1 forward, -1 back
    float strafe  = 0.0f;   // +1 left, -1 right
    bool  moving  = false;
};

// What the game thinks is held, including anything a module is
// currently driving.
inline Input Read(JNIEnv* env) {
    Input in;
    in.forward = (KeyBinds::GetForward(env) ? 1.f : 0.f)
               - (KeyBinds::GetBack(env)    ? 1.f : 0.f);
    in.strafe  = (KeyBinds::GetLeft(env)    ? 1.f : 0.f)
               - (KeyBinds::GetRight(env)   ? 1.f : 0.f);
    in.moving  = (in.forward != 0.f || in.strafe != 0.f);
    return in;
}

// What the HARDWARE says, ignoring our own overrides. A module that
// wants the player's intent rather than the current key state, such
// as Sprint Reset deciding whether you are walking, asks for this.
inline Input ReadPhysical(JNIEnv* env) {
    Input in;
    in.forward = (KeyBinds::PhysForward(env) ? 1.f : 0.f)
               - (KeyBinds::PhysBack(env)    ? 1.f : 0.f);
    in.strafe  = (KeyBinds::PhysLeft(env)    ? 1.f : 0.f)
               - (KeyBinds::PhysRight(env)   ? 1.f : 0.f);
    in.moving  = (in.forward != 0.f || in.strafe != 0.f);
    return in;
}

// The yaw the player is actually travelling along, given the keys.
inline float Angle(float yaw, const Input& in) {
    if (in.forward > 0.f) {
        if (in.strafe > 0.f)      return yaw - 45.f;
        if (in.strafe < 0.f)      return yaw + 45.f;
        return yaw;
    }
    if (in.forward < 0.f) {
        if (in.strafe > 0.f)      return yaw - 135.f;
        if (in.strafe < 0.f)      return yaw + 135.f;
        return yaw + 180.f;
    }
    if (in.strafe > 0.f)          return yaw - 90.f;
    if (in.strafe < 0.f)          return yaw + 90.f;
    return yaw;
}

// Unit vector in world space. False when standing still, in which
// case the outputs are zero rather than left undefined.
inline bool Direction(JNIEnv* env, jobject player,
                      double* outX, double* outZ)
{
    Input in = Read(env);
    if (!in.moving) { *outX = 0.0; *outZ = 0.0; return false; }

    double rad = Angle(Minecraft::GetYaw(env, player), in) * kDegToRad;
    *outX = -std::sin(rad);
    *outZ =  std::cos(rad);
    return true;
}

// Overwrite the horizontal motion vector at a given speed. Every
// caller of this is a module that a prediction anticheat catches on
// the first tick, which is worth saying out loud here rather than
// in three separate files.
inline void SetHorizontal(JNIEnv* env, jobject player, double speed) {
    double dx, dz;
    if (!Direction(env, player, &dx, &dz)) {
        Minecraft::SetMotionX(env, player, 0.0);
        Minecraft::SetMotionZ(env, player, 0.0);
        return;
    }
    Minecraft::SetMotionX(env, player, dx * speed);
    Minecraft::SetMotionZ(env, player, dz * speed);
}

// Current horizontal speed, blocks per tick.
inline double Speed2D(JNIEnv* env, jobject player) {
    double mx = Minecraft::GetMotionX(env, player);
    double mz = Minecraft::GetMotionZ(env, player);
    return std::sqrt(mx * mx + mz * mz);
}

} // namespace Movement
