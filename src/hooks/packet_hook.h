#pragma once
#include <Windows.h>
#include <jni.h>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdio>

// =================================================================
// PacketHook  (variant A: Netty pipeline)
// =================================================================
// WHY THIS EXISTS
//
// The whole client is built on poking fields and polling. That is
// deliberate and it is why so little of it is visible to a server:
// driving KeyBinding.pressed produces exactly the packets a hand
// produces. But three things cannot be done that way, because they
// are ABOUT the packets themselves rather than the game state that
// feeds them:
//
//   * OnServerCorrection. Backtrack and Auto Blockhit both have the
//     method and neither is ever called, because detecting a server
//     position reset means seeing S08PacketPlayerPosLook arrive.
//   * Ping. Backtrack's compensation budget is gated on ping and it
//     currently hardcodes 30ms, because real ping is the round trip
//     of a keep-alive packet.
//   * Silent Aim. A rotation that never moves the camera means
//     rewriting yaw/pitch in the outgoing C03PacketPlayer only. The
//     module cannot do that by writing rotationYaw, which moves the
//     real camera by definition.
//
// WHY NETTY AND NOT MINHOOK
//
// MinHook patches native function addresses. Packets travel through
// Java methods on JIT-compiled code with no stable native address,
// so MinHook cannot reach them. NetworkManager in 1.8 is a netty
// channel, so the supported way in is to insert a ChannelDuplexHandler
// into its pipeline at runtime. That sees both directions, can read,
// rewrite and drop, survives reconnects (re-inject on the new
// channel), and needs no class retransformation.
//
// WHAT THIS FILE IS
//
// The interface and the THREADING CONTRACT, and nothing else yet.
// There is no class resolution and no injection here. Both land in
// the follow-up PR, because the injected handler runs on the netty
// thread and a wrong field id or a stray JNI call there takes the
// game down, not just the overlay. That has to be developed against
// a running game, so it is kept out of this scaffold on purpose.
//
// -----------------------------------------------------------------
// THREADS  (the load-bearing part)
// -----------------------------------------------------------------
// The injected handler runs on netty's I/O thread, NOT our client
// thread, and the client's one rule is that JNI only ever runs on
// the client thread. So the split is:
//
//   netty thread   NoteServerCorrection(), NotePing(): touch only
//                  atomics. Never call a module, never touch JNI
//                  beyond reading the packet object handed to the
//                  handler.
//
//   client thread  PollServerCorrection(), PollPing(): drained once
//                  per tick by ModuleManager, which then does the
//                  JNI-bearing work (broadcasting the correction to
//                  the modules, feeding Backtrack its ping).
//
// The one exception is the outbound rotation rewriter used by Silent
// Aim. It HAS to run synchronously as the C03 packet leaves, so it
// runs on the netty thread inside ApplyRewrite(). The registered
// callback therefore must not touch anything that assumes the client
// thread. This is spelled out again on SetRewriter().
// =================================================================

class PacketHook {
public:
    // The mutable part of an outbound C03PacketPlayer a rewriter is
    // allowed to change. onGround is included because rewriting a
    // rotation without keeping onGround consistent is itself a tell.
    struct Rotation {
        float yaw      = 0.0f;
        float pitch    = 0.0f;
        bool  onGround = false;
    };

    // Returns true if it changed anything, so the handler knows
    // whether it has to write the fields back.
    using RotationRewriter = std::function<bool(Rotation&)>;

private:
    // Injection is live and the handler is in the pipeline.
    inline static std::atomic<bool> s_active{ false };

    // Set on the netty thread when S08 arrives, drained on the
    // client thread. A bool, not a count: several corrections in one
    // tick still mean the same thing, "we were reset, stand down".
    inline static std::atomic<bool> s_correction{ false };

    // Latest measured round-trip in ms, -1 when none since the last
    // drain. Published from the keep-alive round trip on the netty
    // thread.
    inline static std::atomic<int> s_ping{ -1 };

    // Silent Aim's rewriter. Guarded because it is set from the
    // client thread (when the module toggles) and read from the
    // netty thread (as a packet leaves).
    inline static std::mutex        s_rewriteMutex;
    inline static RotationRewriter  s_rewriter;

public:
    // =============================================================
    // Lifecycle  (client thread)
    // =============================================================

    // Fixes the call site. No resolution happens yet: the packet
    // class ids and the channel are looked up at injection time in
    // the follow-up PR, once a world exists and the netty channel is
    // real.
    static bool Init(JNIEnv* /*env*/) {
        return false;
    }

    // FOLLOW-UP PR fills this in:
    //   1. From EntityPlayerSP.sendQueue reach NetHandlerPlayClient,
    //      then its NetworkManager, then the netty Channel.
    //   2. DefineClass a tiny ChannelDuplexHandler whose read/write
    //      call back into RegisterNatives'd stubs here.
    //   3. channel.pipeline().addBefore("packet_handler", ours).
    //   4. s_active.store(true).
    // Re-run on every world load, because a reconnect is a new
    // channel. Returns false here so callers can retry harmlessly.
    static bool Inject(JNIEnv* /*env*/) {
        return false;
    }

    // Remove the handler from the pipeline and forget the rewriter.
    // Safe to call when nothing was ever injected.
    static void Eject(JNIEnv* /*env*/) {
        s_active.store(false);
        ClearRewriter();
        s_correction.store(false);
        s_ping.store(-1);
    }

    static void Shutdown(JNIEnv* env) { Eject(env); }

    static bool IsActive() { return s_active.load(); }

    // =============================================================
    // Netty-thread notifiers
    // =============================================================
    // Called from the injected handler. Atomics only: safe from any
    // thread, and deliberately do NO JNI and touch NO module state.

    static void NoteServerCorrection() {
        s_correction.store(true);
    }

    static void NotePing(int ms) {
        if (ms >= 0) s_ping.store(ms);
    }

    // =============================================================
    // Client-thread drains
    // =============================================================
    // Called once per tick by ModuleManager, which then does the
    // JNI-bearing follow-through on the client thread.

    // True at most once per correction. Consumes the flag.
    static bool PollServerCorrection() {
        return s_correction.exchange(false);
    }

    // Writes the latest ping and returns true if there was a fresh
    // one since the last drain.
    static bool PollPing(int* out) {
        int v = s_ping.exchange(-1);
        if (v < 0) return false;
        if (out) *out = v;
        return true;
    }

    // =============================================================
    // Outbound rotation rewrite  (Silent Aim)
    // =============================================================

    // Registered by the module from the CLIENT thread. The callback
    // itself is invoked on the NETTY thread as a C03 packet leaves,
    // so it must be self-contained: read the fields it is given,
    // decide, and return. It must not call into Minecraft, the
    // module manager, or anything else that assumes the client
    // thread.
    static void SetRewriter(RotationRewriter fn) {
        std::lock_guard<std::mutex> lock(s_rewriteMutex);
        s_rewriter = std::move(fn);
    }

    static void ClearRewriter() {
        std::lock_guard<std::mutex> lock(s_rewriteMutex);
        s_rewriter = nullptr;
    }

    static bool HasRewriter() {
        std::lock_guard<std::mutex> lock(s_rewriteMutex);
        return (bool)s_rewriter;
    }

    // Invoked by the injected handler on the netty thread. Returns
    // true when the rotation was changed and the handler must write
    // it back into the packet.
    static bool ApplyRewrite(Rotation& r) {
        std::lock_guard<std::mutex> lock(s_rewriteMutex);
        if (!s_rewriter) return false;
        return s_rewriter(r);
    }
};
