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

| Key | Action |
|-----|--------|
| INSERT | Toggle menu |
| DELETE | Eject |
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
   `AttachCurrentThread` on our own client thread.
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
6. **Client loop.** 20 TPS to match Minecraft, since module delays are in ticks.

## Threading

One rule: **JNI only ever runs on the client thread.**

`ModuleManager::Tick` wraps each tick in a JNI local frame so the reference
table cannot overflow. The render thread draws the menu and ESP but never calls
JNI. Anything the UI needs a `JNIEnv` for (toggles, loading a profile) is pushed
onto an action queue and executed on the client thread. ESP publishes a
plain-data snapshot under a mutex for the renderer to read.

## Profiles

The Configs tab loads a profile: it enables the right modules, forces the
dangerous ones off, and sets values tuned for that server's anticheat.

| Profile | For |
|---------|-----|
| Minemen (AGC) | Duel server on Karhu-based prediction. Keyboard-level only |
| Polar | MineBlaze, Pika, GommeHD. Legit combat plus bridging |
| Legit | Minimum values. Safe under spectator and on video |
| Blatant | No-anticheat servers only |

Profiles bind by name to settings each module registers with `Bind()`. Adding a
new tunable is one `Bind("Name", &field)` call in the constructor.

## Module status

| Module | Category | State |
|--------|----------|-------|
| Velocity | Combat | Working. Direct plus legit jump/strafe |
| Sprint Reset | Combat | Working. 5 techniques via real keybinds |
| Auto Blockhit | Combat | Working. Drives `keyBindUseItem` |
| Aim Assist | Combat | Working |
| Hit Select | Combat | Working |
| Click Assist | Combat | Working. Butterfly, drag, jitter |
| Kill Aura | Combat | Working. Loud, detected everywhere |
| **Backtrack** | Combat | **Inert.** Needs a Netty packet hook |
| Bridge Assist | Movement | Working |
| Sprint | Movement | Working |
| No Jump Delay | Movement | Working |
| Speed | Movement | Working. Detected by prediction ACs |
| Fly | Movement | Working. Detected by prediction ACs |
| ESP | Visual | Working. Own view-projection, not GL matrices |
| Fullbright | Visual | Working |

## Known gaps

- **Backtrack** has no packet hook, so it does nothing. Its tuning and safety
  logic are written and waiting; the interception is not. The menu says so.
- **Health** is read through `getHealth()`. If a Lunar build strips that method
  the ESP health bar falls back to full.
- **Speed and Fly** will not survive a prediction anticheat. They are there for
  unprotected servers.
- Config profiles live in code. There is no save/load to disk yet.

## Troubleshooting

The DLL opens a console. If class resolution fails it dumps every loaded class
signature so you can find the obfuscated names by hand, then extend the
candidate lists in the module's `JvmtiUtil::FindField` call.

Injecting at the main menu is fine. Startup retries for 60 seconds, and
keybinds and the entity list keep retrying quietly until a world loads.

If a module prints "unresolved" in its settings panel, that lookup failed and
the module is inert rather than silently doing nothing.
