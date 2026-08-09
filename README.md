# Phantom Client

Internal DLL for Lunar Client 1.8.9. JNI + OpenGL hook + ImGui overlay.

## Architecture

```
src/
├── dllmain.cpp              # DLL entry, JVM attach, main loop
├── jni/
│   ├── jni_helper.h         # JVM attach via JNI_GetCreatedJavaVMs
│   └── class_resolver.h     # JVMTI class enumeration (bypasses Lunar's classloader)
├── mappings/
│   └── mcp189.h             # MCP 1.8.9 SRG/Notch field & method names
├── mc/
│   └── minecraft.h          # JNI wrapper: player pos, rotation, motion, etc.
├── hooks/
│   └── gl_hook.h            # wglSwapBuffers hook via MinHook + ImGui init
├── modules/
│   ├── module.h             # Base module class
│   ├── module_manager.h     # Registry, keybind handler, tick loop
│   ├── combat/
│   │   ├── aim_assist.h     # Smooth aim correction (FOV, speed, target mode)
│   │   ├── kill_aura.h      # Auto-attack with randomized CPS
│   │   └── velocity.h       # Knockback reduction (reduce/cancel/reverse)
│   ├── movement/
│   │   ├── speed.h          # Strafe/BHop speed boost
│   │   ├── sprint.h         # Always sprint + omni-sprint
│   │   └── fly.h            # Vanilla fly with anti-kick
│   └── visual/
│       ├── esp.h            # Player ESP (box, health, distance)
│       └── fullbright.h     # Gamma override
└── gui/
    └── menu.h               # ImGui menu with dark theme, HUD overlay
```

## How it works

1. **DLL Injection**: Inject into `javaw.exe` (Lunar Client) via any injector
2. **JVM Attach**: `JNI_GetCreatedJavaVMs` from `jvm.dll` to get the running JVM
3. **Class Resolution**: JVMTI `GetLoadedClasses` to enumerate all classes (Lunar's custom classloader prevents `FindClass`)
4. **Field Identification**: Match classes by their unique fields (e.g. Minecraft has `thePlayer`)
5. **OpenGL Hook**: MinHook on `wglSwapBuffers` to create a second GL context for ImGui rendering
6. **WndProc Hook**: Intercept input for ImGui when menu is open

## Keybinds

| Key | Action |
|-----|--------|
| INSERT | Toggle menu |
| DELETE | Eject DLL |
| R | Aim Assist |
| K | Kill Aura |
| B | Velocity |
| F | Speed |
| CTRL | Sprint |
| G | Fly |
| X | ESP |
| H | Fullbright |

## Build

Requirements:
- Visual Studio 2022 with C++ workload
- CMake 3.20+
- JDK 8 (for JNI headers)

```bash
mkdir build && cd build
cmake .. -DJDK_PATH="C:/Program Files/Java/jdk1.8.0_202"
cmake --build . --config Release
```

Output: `build/Release/PhantomClient.dll`

## Dependencies (auto-fetched by CMake)

- [MinHook](https://github.com/TsudaKageWorker/minhook) - trampoline hooking
- [Dear ImGui](https://github.com/ocornut/imgui) - GUI framework

## TODO

- [ ] Entity list iteration via `world.playerEntities` JNI List
- [ ] ESP screen projection (gluProject)
- [ ] KillAura attack packets via `PlayerControllerMP.attackEntity()`
- [ ] Packet-level Velocity (hook S12PacketEntityVelocity)
- [ ] Config save/load system
- [ ] More modules: Reach, AutoClicker, Criticals, NoFall, Scaffold

## Mapping notes

Lunar 1.8.9 can use Notch, SRG, or MCP names depending on the build. The class resolver tries SRG first, then MCP, then Notch. If fields aren't found, use `ClassResolver::DumpAllClasses()` to print all loaded class signatures and reverse-engineer the correct names.

For generating fresh C++ mapping headers, see [hiraeeth/mcp-generator](https://github.com/hiraeeth/mcp-generator).
