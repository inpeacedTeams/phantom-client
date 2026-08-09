#pragma once
#include <jni.h>
#include <cmath>
#include <mutex>
#include <chrono>

#include "../mc/minecraft.h"
#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"

// =================================================================
// Camera
// =================================================================
// One camera snapshot, shared by everything that draws in world
// space. ESP and the Backtrack visualiser both need it, and two
// copies of a projection routine is two chances to get it subtly
// wrong in different ways.
//
// WHY NOT READ THE GL MATRICES
// Reading GL_MODELVIEW_MATRIX from the wglSwapBuffers hook returns
// the orthographic GUI matrix, because Minecraft has already
// finished both the world pass and the GUI pass by then. Projecting
// world coordinates through it gives garbage. Rebuilding the camera
// from eye position, yaw, pitch and FOV is deterministic and does
// not care when we run.
//
// THREADING
// Update() runs on the client thread and writes under a mutex.
// Get() runs on the render thread and only reads.
// =================================================================

struct CameraView {
    double x = 0, y = 0, z = 0;          // eye position this tick
    double prevX = 0, prevY = 0, prevZ = 0;
    float  yaw = 0, pitch = 0;
    float  fov = 90.f;
    bool   valid = false;
    std::chrono::steady_clock::time_point stamp;

    // Eye position interpolated toward the current tick
    void EyeAt(float alpha, double* ox, double* oy, double* oz) const {
        *ox = prevX + (x - prevX) * alpha;
        *oy = prevY + (y - prevY) * alpha;
        *oz = prevZ + (z - prevZ) * alpha;
    }
};

class Camera {
private:
    inline static CameraView s_view;
    inline static std::mutex s_mutex;
    inline static jfieldID   s_fFov = nullptr;
    inline static bool       s_fovResolved = false;

public:
    // Client thread, once per tick
    static void Update(JNIEnv* env) {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_view.valid = false;
            return;
        }

        if (!s_fovResolved) {
            if (ClassResolver::gameSettings) {
                s_fFov = JvmtiUtil::FindField(env, ClassResolver::gameSettings,
                    { "field_74334_X", "fovSetting" });
            }
            s_fovResolved = true;
        }

        float fov = 90.f;
        if (s_fFov) {
            jobject gs = Minecraft::GetGameSettings(env);
            if (gs) fov = env->GetFloatField(gs, s_fFov);
        }

        CameraView v;
        v.x     = Minecraft::GetPosX(env, player);
        v.y     = Minecraft::GetPosY(env, player) + 1.62;
        v.z     = Minecraft::GetPosZ(env, player);
        v.prevX = Minecraft::GetPrevPosX(env, player);
        v.prevY = Minecraft::GetPrevPosY(env, player) + 1.62;
        v.prevZ = Minecraft::GetPrevPosZ(env, player);
        v.yaw   = Minecraft::GetYaw(env, player);
        v.pitch = Minecraft::GetPitch(env, player);
        v.fov   = fov;
        v.valid = true;
        v.stamp = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(s_mutex);
        s_view = v;
    }

    static void Invalidate() {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_view.valid = false;
    }

    // Render thread
    static CameraView Get() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_view;
    }

    // How far through the current tick we are, 0 to 1. Used to
    // smooth 20 TPS data across a 200 FPS render.
    static float Alpha(const CameraView& v) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - v.stamp).count();
        float a = (float)ms / 50.0f;
        if (a < 0.f) a = 0.f;
        if (a > 1.f) a = 1.f;
        return a;
    }

    // World point to screen point. False when behind the camera.
    static bool Project(const CameraView& v,
                        double wx, double wy, double wz,
                        double camX, double camY, double camZ,
                        float screenW, float screenH,
                        float* outX, float* outY)
    {
        const double DEG = 3.14159265358979 / 180.0;

        double dx = wx - camX;
        double dy = wy - camY;
        double dz = wz - camZ;

        // Minecraft renders rotate(pitch, X) then rotate(yaw+180, Y)
        double ya = (v.yaw + 180.0) * DEG;
        double cy = std::cos(ya), sy = std::sin(ya);
        double x1 =  dx * cy + dz * sy;
        double y1 =  dy;
        double z1 = -dx * sy + dz * cy;

        double pa = v.pitch * DEG;
        double cp = std::cos(pa), sp = std::sin(pa);
        double x2 = x1;
        double y2 = y1 * cp - z1 * sp;
        double z2 = y1 * sp + z1 * cp;

        double depth = -z2;              // camera looks down -Z
        if (depth < 0.05) return false;

        double aspect = (screenH > 0) ? ((double)screenW / (double)screenH) : 1.777;
        double f = 1.0 / std::tan((v.fov * 0.5) * DEG);

        double ndcX = (f / aspect) * (x2 / depth);
        double ndcY = f * (y2 / depth);

        *outX = (float)((ndcX * 0.5 + 0.5) * screenW);
        *outY = (float)((1.0 - (ndcY * 0.5 + 0.5)) * screenH);
        return true;
    }
};
