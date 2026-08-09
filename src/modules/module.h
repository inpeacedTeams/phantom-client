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
    int            m_keybind;

    // Read from the render thread while the client thread writes it,
    // so it has to be atomic rather than a plain bool.
    std::atomic<bool> m_enabled{ false };

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
        : m_name(name), m_description(desc), m_category(cat), m_keybind(key) {}

    virtual ~Module() = default;

    virtual void OnTick(JNIEnv* env) = 0;
    virtual void OnEnable(JNIEnv*) {}
    virtual void OnDisable(JNIEnv*) {}
    virtual void RenderSettings() {}

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
    int  GetKeybind() const                   { return m_keybind; }
    void SetKeybind(int key)                  { m_keybind = key; }
    bool IsEnabled() const                    { return m_enabled.load(); }
};
