# Phantom Client

Internal DLL for Lunar Client 1.8.9. C++ with JNI and JVMTI, ImGui overlay
rendered through a `wglSwapBuffers` hook.

## Build

Visual Studio 2022 (C++ workload), CMake 3.20+, JDK 8 for `jni.h` and `jvmti.h`.

```bash
mkdir build && cd build
cmake .. -DJDK_PATH="C:/Program Files/Java/jdk1.8.0_202"
cmake --build . --config Release
```

Output: `build/Release/PhantomClient.dll`. Inject into `javaw.exe`.
MinHook and Dear ImGui are fetched by CMake.

Every module is a header-only class, so the whole client is one translation
unit: `src/dllmain.cpp` plus the ImGui sources.

## Keybinds

Every module bind below is a **default** and can be rebound live from the menu:
open a module and click its keybind chip, then press the key (BACKSPACE clears
it, ESC cancels). Binds are saved with the config. INSERT and DELETE are
reserved and cannot be bound to a module.

| Key | Action |
|-----|--------|
| INSERT | Toggle menu (reserved) |
| DELETE | Eject (reserved) |
| B | Velocity |
| R | Aim Assist |
| K | Kill Aura |
| F | Speed |
| G | Fly |
| CTRL | Sprint |
| X | ESP |
| H | Fullbright |

## How it works

1. **JVM attach.** `JNI_GetCreatedJavaVMs` out of `jvm.dll`, then
   `AttachCurrentThreadAsDaemon`. Daemon matters: a non-daemon thread keeps the
   JVM alive, so closing Minecraft without ejecting first would leave `javaw`
   running as a zombie.
2. **Class resolution.** Lunar uses a custom classloader, so `FindClass` cannot
   see Minecraft classes. We enumerate every loaded class through JVMTI and
   identify them by the fields they declare.
3. **Field lookup.** `GetFieldID` needs an exact signature and Minecraft's are
   obfuscated: `thePlayer` is `Lbew;`, not anything guessable. `JvmtiUtil` reads
   the real signature from JVMTI and builds the ID from that, so lookups survive
   Lunar updates.
4. **Input simulation.** Modules drive `GameSettings.keyBind*.pressed` rather
   than entity flags. This matters twice over: `setSprinting` and `setSneaking`
   are recomputed from the keys every tick and would be overwritten, and driving
   the real keybind makes the outgoing packets identical to a human's.
5. **Overlay.** MinHook on `wglSwapBuffers`. We create a second GL context,
   share lists with the game's, draw, then restore the original context.

## Timing

Two clocks, because one is not enough.

**Module loop, 20 TPS.** Matches Minecraft, since every module expresses its
delays in ticks. Anything faster made a "2 tick" delay expire in 2ms.

**Click scheduler, 1ms.** Clicks cannot come from the tick loop: the smallest
gap it can express is 50ms, so every butterfly pair collapsed into a dead-flat
20 CPS stream with near-zero variance, which is the clearest machine signature
there is. `ClickScheduler` runs its own thread with `timeBeginPeriod(1)` plus a
short spin for the final 2ms, and asks the module for each gap through a
callback.

Every click in the client leaves through that scheduler, including the one-off
from Hit Select. Two sources firing `SendInput` independently can land
microseconds apart, and a sub-20ms interval is the one thing no hand produces,
so the scheduler holds a shared floor and drops anything that would break it.

## Tick order

Backtrack rewrites entity positions, so the order inside a tick is load-bearing:

1. Push the JNI local frame and invalidate the entity cache.
2. **Backtrack restores** every rewound entity to its true position.
3. Modules run. The entity scan happens here, so everything else and the ESP
   see the real world rather than the rewound one.
4. Backtrack records the true positions and rewinds again.

Backtrack holds its targets as global refs, because the restore in step 2 has to
reach entities whose local refs died with the previous frame.

## Threading

One rule: **JNI only ever runs on the client thread.**

`ModuleManager::Tick` wraps each tick in a JNI local frame so the reference
table cannot overflow. The entity scan runs once per tick and is shared, rather
than five modules each rebuilding it.

The render thread draws the menu and ESP but never calls JNI. Anything the UI
needs a `JNIEnv` for (toggles, loading a profile) goes onto an action queue and
runs on the client thread. ESP publishes a plain-data snapshot under a mutex.

The click scheduler is a third thread. It only touches its own state and
`SendInput`, and it is stopped before the modules are destroyed because its
callback captures a module pointer.

**Eject ordering.** `GLHook::Remove()` raises a shutdown flag, waits for any
in-flight frame to leave the hook, and only then destroys ImGui. It runs before
`ModuleManager::Shutdown()`, otherwise the render thread would walk a module
list that is being freed underneath it.

**Held keys.** Modules hold real keybinds down. Leaving a world, disabling a
module and ejecting all release them, so a disconnect mid-fight cannot leave you
sprinting into a wall in the next game. `KeyBinds::Reconcile` runs at the end of
every tick as a backstop: any key we drove but are no longer driving is put back
in line with the hardware, so a missed release costs one tick instead of the
rest of the fight.

## Configs and presets

The Configs tab holds two different things.

**Presets** are built in and read-only. Each is an opinionated starting point
tuned for a particular anticheat; applying one overwrites your current settings.

| Preset | For |
|--------|-----|
| Minemen (AGC) | Duel server on Karhu-based prediction. Keyboard-level only |
| Polar | MineBlaze, Pika, GommeHD. Legit combat plus bridging |
| Legit | Minimum values. Safe under spectator and on video |
| Blatant | No-anticheat servers only |

Presets bind by name to settings each module registers with `Bind()`. Adding a
new tunable is one `Bind("Name", &field)` call in the constructor.

**Configs** are yours, saved to disk and loaded by name. They live in
`%APPDATA%\Phantom` as plain text (one `key=value` per line, format v2) so you
can open and edit one by hand. `default.cfg` is written automatically at eject
and once a minute while you play, so doing nothing at all still gets you
persistence and a crash never costs a session. Writes go through a temp file and
an atomic rename, so a crash mid-write cannot leave a truncated config.

Loading is forward and backward compatible: unknown keys are ignored, missing
keys keep the module's default, malformed lines are skipped and counted, and
values are clamped to each setting's range on the way in. A config from a build
with a module or setting this one lacks loads fine; a config from an older build
is migrated where a key has been replaced.

## Module status

| Module | Category | State |
|--------|----------|-------|
| Velocity | Combat | Working. Direct plus legit jump/strafe |
| Sprint Reset | Combat | Working. 5 techniques via real keybinds |
| Auto Blockhit | Combat | Working. Drives `keyBindUseItem` |
| Aim Assist | Combat | Working |
| Hit Select | Combat | Working. Emits through the shared click floor |
| Auto Clicker | Combat | Working. Butterfly, drag, jitter on the 1ms thread |
| Kill Aura | Combat | Working. Loud, detected everywhere |
| Backtrack | Combat | Working. Position rewind, no packet hook |
| Bridge Assist | Movement | Working |
| Sprint | Movement | Working |
| No Jump Delay | Movement | Working |
| Speed | Movement | Working. Detected by prediction ACs |
| Fly | Movement | Working. Detected by prediction ACs |
| ESP | Visual | Working. Own view-projection, not GL matrices |
| Fullbright | Visual | Working |

When a module cannot resolve something it needs (a keybind, `getHealth()`, the
click counter), it says so as an on-screen notice and its settings panel shows a
callout, rather than silently doing nothing. A module that throws repeatedly is
switched off on its own and the rest of the client keeps running.

## Known gaps

- **Backtrack rewinds only between our ticks**, because there is no packet hook
  to hold updates at the source. On a low-ping duel server there is little
  natural jitter to hide inside, so keep the band short. It is the least
  reliable module here.
- **No lagback handler.** `OnServerCorrection` exists on Backtrack and Auto
  Blockhit but nothing calls it: detecting a server position reset needs the
  packet hook. Until then, both keep running through a correction.
- **Health** is read through `getHealth()`. If a Lunar build strips that method
  the ESP health bar falls back to full.
- **Speed and Fly** will not survive a prediction anticheat. They are there for
  unprotected servers.

## Troubleshooting

In a **debug build** (or a release built with `PHANTOM_CONSOLE` defined) the DLL
opens a console. If class resolution fails it dumps every loaded class signature
so you can find the obfuscated names by hand, then extend the candidate lists in
that module's `JvmtiUtil::FindField` call. A normal release build has no
console: everything the player needs arrives as an on-screen notification.

Injecting at the main menu is fine. Startup retries for 60 seconds, and keybinds
and the entity list keep retrying quietly until a world loads.

If a module shows an "unresolved" callout in its settings panel, that lookup
failed and the module is inert rather than silently doing nothing.
