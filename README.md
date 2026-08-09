# Phantom Client

Internal DLL for Lunar Client 1.8.9. C++ / JNI / JVMTI with an ImGui overlay
rendered through a `wglSwapBuffers` hook.

## Build

Requirements: Visual Studio 2022 (C++ workload), CMake 3.20+, JDK 8 for the
`jni.h` and `jvmti.h` headers.

```bash
mkdir build && cd build
cmake .. -DJDK_PATH="C:/Program Files/Java/jdk1.8.0_202"
cmake --build . --config Release
```

Output: `build/Release/PhantomClient.dll`. Inject into `javaw.exe`.
MinHook and Dear ImGui are fetched automatically.

Every module is a header-only class, so the client compiles as a single
translation unit. The per-module `.cpp` files in the tree are legacy stubs and
are not part of the build.

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

1. **JVM attach.** `JNI_GetCreatedJavaVMs` from `jvm.dll`, then
   `AttachCurrentThread` on our client thread.
2. **Class resolution.** Lunar uses a custom classloader, so `FindClass` cannot
   see Minecraft classes. We enumerate every loaded class through JVMTI and
   identify them by the fields they declare.
3. **Field lookup.** `GetFieldID` needs an exact signature, and Minecraft's are
   obfuscated (`thePlayer` is `Lbew;`, not the readable name). `JvmtiUtil` reads
   the real signature from JVMTI and builds the ID from that, so lookups work
   across Lunar builds.
4. **Overlay.** MinHook on `wglSwapBuffers`. We create a second GL context,
   share lists with the game's, draw ImGui, then restore the original context.
5. **Client loop.** Runs at 20 TPS to match Minecraft, since every module
   expresses its delays in ticks.

## Threading

One rule: **JNI only ever runs on the client thread.**

`ModuleManager::Tick` wraps each tick in a JNI local frame, so the reference
table cannot overflow. The render thread draws the menu and ESP but never calls
JNI. Menu toggles are queued and applied on the client thread, because
`OnEnable`/`OnDisable` need a valid `JNIEnv`. ESP publishes a plain-data
snapshot under a mutex for the renderer to read.

## Module status

| Module | Category | State |
|--------|----------|-------|
| Velocity | Combat | Working. Direct + legit modes |
| Sprint Reset | Combat | Working. 6 techniques |
| Auto Blockhit | Combat | Working. Drives the real `keyBindUseItem` |
| Aim Assist | Combat | Working |
| Hit Select | Combat | Working |
| Click Assist | Combat | Working. Butterfly / drag / jitter |
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

- **Backtrack** has no packet hook, so it does nothing. The tuning layer and
  the safety logic are written and waiting; the interception is not.
- **Config profiles** under `src/config/` are documented constants. Nothing
  loads them into the modules yet.
- **Health** is read through `getHealth()`. If a Lunar build strips that method
  the ESP health bar falls back to full.
- Fly and Speed will not survive a prediction-based anticheat. They exist for
  unprotected servers.

## Troubleshooting

The DLL opens a console. If class resolution fails it dumps every loaded class
signature so you can find the obfuscated names by hand, then extend the
candidate lists in `src/mappings/mcp189.h`.

Injecting at the main menu is fine: startup retries for 60 seconds while it
waits for the game to finish loading.
