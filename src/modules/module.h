#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <jni.h>

enum class ModuleCategory {
    COMBAT,
    MOVEMENT,
    VISUAL,
    PLAYER,
    MISC
};

// -----------------------------------------------------------------
// Setting
// -----------------------------------------------------------------
// A named pointer to a member field. Modules register their tunables
// in the constructor so config profiles can set them by name without
// every module needing bespoke apply code.
//
// Registering a setting is NOT the same as showing it. Everything
// stays bound so profiles can tune it, but only the handful that
// change how a module feels are drawn by default. The rest live
// behind Advanced.
// -----------------------------------------------------------------
struct Setting {
    enum class Type { Bool, Int, Float };

    std::string name;
    Type type;
    void* ptr;

    bool  AsBool()  const { return *(bool*)ptr; }
    int   AsInt()   const { return *(int*)ptr; }
    float AsFloat() const { return *(float*)ptr; }
};

class Module {
protected:
    std::string    m_name;
    std::string    m_description;
    ModuleCategory m_category;

    // Both of these are written on one thread and read on another.
    //
    //   m_enabled  the menu queues a toggle, the tick loop applies it
    //   m_keybind  the picker writes it from the RENDER thread while
    //              the tick loop is reading it to poll hotkeys
    //
    // An int tear is not a real hazard on x86, but a torn read here
    // would fire the wrong module, and the fix costs nothing.
    std::atomic<bool> m_enabled{ false };
    std::atomic<int>  m_keybind{ 0 };

    std::vector<Setting> m_settings;

    void Bind(const char* name, bool* p) {
        m_settings.push_back({ name, Setting::Type::Bool, (void*)p });
    }
    void Bind(const char* name, int* p) {
        m_settings.push_back({ name, Setting::Type::Int, (void*)p });
    }
    void Bind(const char* name, float* p) {
        m_settings.push_back({ name, Setting::Type::Float, (void*)p });
    }

public:
    Module(const std::string& name, const std::string& desc,
           ModuleCategory cat, int key = 0)
        : m_name(name), m_description(desc), m_category(cat)
    {
        m_keybind.store(key);
    }

    virtual ~Module() = default;

    virtual void OnTick(JNIEnv* env) = 0;
    virtual void OnEnable(JNIEnv*) {}
    virtual void OnDisable(JNIEnv*) {}

    // ---- Panel ----
    // RenderSettings is the everyday panel: the mode, and the two or
    // three values worth touching. Keep it short enough to read at a
    // glance.
    //
    // RenderAdvanced is everything else. A module with thirty knobs
    // is not more powerful than one with four, it is just harder to
    // set up, and most people never open it.
    virtual void RenderSettings() {}
    virtual void RenderAdvanced() {}
    virtual bool HasAdvanced() const { return false; }

    // A one-line live state string for the collapsed row, so the
    // panel does not have to be open to see what a module is doing.
    virtual const char* StatusLine() const { return nullptr; }

    void Toggle(JNIEnv* env) {
        SetEnabled(!m_enabled.load(), env);
    }

    void SetEnabled(bool enabled, JNIEnv* env) {
        if (m_enabled.load() == enabled) return;
        m_enabled.store(enabled);
        if (!env) return;
        if (enabled) OnEnable(env);
        else         OnDisable(env);
    }

    // ---- Config access ----
    bool SetValue(const std::string& setting, float value) {
        for (auto& s : m_settings) {
            if (s.name != setting) continue;
            switch (s.type) {
                case Setting::Type::Bool:  *(bool*)s.ptr  = (value != 0.f); return true;
                case Setting::Type::Int:   *(int*)s.ptr   = (int)value;     return true;
                case Setting::Type::Float: *(float*)s.ptr = value;          return true;
            }
        }
        return false;
    }

    const std::vector<Setting>& GetSettings() const { return m_settings; }

    const std::string& GetName() const        { return m_name; }
    const std::string& GetDescription() const { return m_description; }
    ModuleCategory     GetCategory() const    { return m_category; }
    int  GetKeybind() const                   { return m_keybind.load(); }
    void SetKeybind(int key)                  { m_keybind.store(key); }
    bool IsEnabled() const                    { return m_enabled.load(); }
};
