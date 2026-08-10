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
// -----------------------------------------------------------------
// WHY BOXES USED TO VANISH AND FLICKER
// -----------------------------------------------------------------
// The old code projected the eight corners of a bounding box one at
// a time, threw away any that were behind the camera, and gave up
// if fewer than four survived.
//
// That is wrong in the exact situation that matters most. Stand
// next to someone and the lower corners of their box fall behind
// the near plane while their head is still on screen. Corners drop
// out, the 2D bounds are computed from whatever is left, and the
// box either shrinks to a sliver or disappears entirely. Turn a few
// degrees and it pops back. That is the flicker.
//
// A bounding box is not eight points, it is twelve EDGES. Clipping
// each edge against the near plane keeps the silhouette correct no
// matter how much of the box is behind you, because the clipped
// intersection point is still a real point on the box.
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

// A world point already rotated into camera space.
//   x right, y up, depth forward. Behind the camera means depth < 0.
struct ViewPoint {
    double x = 0, y = 0, depth = 0;
};

// Screen-space bounds of a projected box
struct ScreenBox {
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool  valid = false;

    float Width()  const { return maxX - minX; }
    float Height() const { return maxY - minY; }
    float CenterX() const { return (minX + maxX) * 0.5f; }
    float CenterY() const { return (minY + maxY) * 0.5f; }
};

class Camera {
private:
    inline static CameraView s_view;
    inline static std::mutex s_mutex;
    inline static jfieldID   s_fFov = nullptr;
    inline static bool       s_fovResolved = false;

    // Anything closer than this is treated as behind the camera.
    // Matching Minecraft's own 0.05 near plane keeps the geometry
    // consistent with what the game actually drew.
    static constexpr double kNear = 0.05;

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

    // -------------------------------------------------------------
    // World point into camera space.
    //
    // Minecraft renders rotate(pitch, X) then rotate(yaw+180, Y),
    // so the same order is reproduced here.
    // -------------------------------------------------------------
    static ViewPoint ToView(const CameraView& v,
                            double wx, double wy, double wz,
                            double camX, double camY, double camZ)
    {
        const double DEG = 3.14159265358979 / 180.0;

        double dx = wx - camX;
        double dy = wy - camY;
        double dz = wz - camZ;

        double ya = (v.yaw + 180.0) * DEG;
        double cy = std::cos(ya), sy = std::sin(ya);
        double x1 =  dx * cy + dz * sy;
        double y1 =  dy;
        double z1 = -dx * sy + dz * cy;

        double pa = v.pitch * DEG;
        double cp = std::cos(pa), sp = std::sin(pa);

        ViewPoint p;
        p.x     = x1;
        p.y     = y1 * cp - z1 * sp;
        p.depth = -(y1 * sp + z1 * cp);   // camera looks down -Z
        return p;
    }

    // Camera space to pixels. Caller must ensure depth >= kNear.
    static void ViewToScreen(const CameraView& v, const ViewPoint& p,
                             float screenW, float screenH,
                             float* outX, float* outY)
    {
        const double DEG = 3.14159265358979 / 180.0;

        double aspect = (screenH > 0) ? ((double)screenW / (double)screenH) : 1.777;
        double f = 1.0 / std::tan((v.fov * 0.5) * DEG);

        double ndcX = (f / aspect) * (p.x / p.depth);
        double ndcY = f * (p.y / p.depth);

        *outX = (float)((ndcX * 0.5 + 0.5) * screenW);
        *outY = (float)((1.0 - (ndcY * 0.5 + 0.5)) * screenH);
    }

    // World point to screen point. False when behind the camera.
    static bool Project(const CameraView& v,
                        double wx, double wy, double wz,
                        double camX, double camY, double camZ,
                        float screenW, float screenH,
                        float* outX, float* outY)
    {
        ViewPoint p = ToView(v, wx, wy, wz, camX, camY, camZ);
        if (p.depth < kNear) return false;
        ViewToScreen(v, p, screenW, screenH, outX, outY);
        return true;
    }

    // -------------------------------------------------------------
    // Screen bounds of an axis-aligned world box.
    //
    // Walks the twelve edges and clips each against the near plane,
    // so a box the camera is standing inside still yields correct
    // bounds instead of collapsing. This is what stops the box from
    // popping in and out as you turn.
    // -------------------------------------------------------------
    static ScreenBox ProjectBox(const CameraView& v,
                                double minWX, double minWY, double minWZ,
                                double maxWX, double maxWY, double maxWZ,
                                double camX, double camY, double camZ,
                                float screenW, float screenH)
    {
        ScreenBox out;

        const double cx[8] = { minWX, maxWX, minWX, maxWX, minWX, maxWX, minWX, maxWX };
        const double cy[8] = { minWY, minWY, maxWY, maxWY, minWY, minWY, maxWY, maxWY };
        const double cz[8] = { minWZ, minWZ, minWZ, minWZ, maxWZ, maxWZ, maxWZ, maxWZ };

        ViewPoint vp[8];
        for (int i = 0; i < 8; i++)
            vp[i] = ToView(v, cx[i], cy[i], cz[i], camX, camY, camZ);

        // Every corner behind the near plane means the box really is
        // behind us. Nothing to draw.
        bool anyFront = false;
        for (int i = 0; i < 8; i++) if (vp[i].depth >= kNear) { anyFront = true; break; }
        if (!anyFront) return out;

        static const int kEdges[12][2] = {
            {0,1},{1,3},{3,2},{2,0},      // bottom face
            {4,5},{5,7},{7,6},{6,4},      // top face
            {0,4},{1,5},{2,6},{3,7}       // uprights
        };

        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        bool have = false;

        auto accumulate = [&](const ViewPoint& p) {
            float sx, sy;
            ViewToScreen(v, p, screenW, screenH, &sx, &sy);
            if (sx < minX) minX = sx;
            if (sx > maxX) maxX = sx;
            if (sy < minY) minY = sy;
            if (sy > maxY) maxY = sy;
            have = true;
        };

        for (int e = 0; e < 12; e++) {
            ViewPoint a = vp[kEdges[e][0]];
            ViewPoint b = vp[kEdges[e][1]];

            bool aOk = a.depth >= kNear;
            bool bOk = b.depth >= kNear;

            if (aOk && bOk) {
                accumulate(a);
                accumulate(b);
                continue;
            }
            if (!aOk && !bOk) continue;   // whole edge is behind

            // One end crosses the plane. Slide it up to the plane and
            // use the intersection, which is still a point on the box.
            if (!aOk) { ViewPoint t = a; a = b; b = t; }

            double span = a.depth - b.depth;
            double t = (span != 0.0) ? (a.depth - kNear) / span : 0.0;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;

            ViewPoint clip;
            clip.x     = a.x + (b.x - a.x) * t;
            clip.y     = a.y + (b.y - a.y) * t;
            clip.depth = kNear;

            accumulate(a);
            accumulate(clip);
        }

        if (!have) return out;

        out.minX = minX; out.minY = minY;
        out.maxX = maxX; out.maxY = maxY;
        out.valid = true;
        return out;
    }

    // Is any part of this box on screen? Cheap reject before the
    // caller starts building geometry.
    static bool OnScreen(const ScreenBox& b, float screenW, float screenH,
                         float margin = 64.0f)
    {
        if (!b.valid) return false;
        if (b.maxX < -margin || b.minX > screenW + margin) return false;
        if (b.maxY < -margin || b.minY > screenH + margin) return false;
        return true;
    }
};
