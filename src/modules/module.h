#pragma once
#include <string>
#include <jni.h>

enum class ModuleCategory {
    COMBAT,
    MOVEMENT,
    VISUAL,
    PLAYER,
    MISC
};

class Module {
protected:
    std::string m_name;
    std::string m_description;
    ModuleCategory m_category;
    int m_keybind; // Virtual key code, 0 = none
    bool m_enabled = false;

public:
    Module(const std::string& name, const std::string& desc, ModuleCategory cat, int key = 0)
        : m_name(name), m_description(desc), m_category(cat), m_keybind(key) {}

    virtual ~Module() = default;

    // Called every tick when enabled
    virtual void OnTick(JNIEnv* env) = 0;

    // Called when toggled on/off
    virtual void OnEnable(JNIEnv* env) {}
    virtual void OnDisable(JNIEnv* env) {}

    // ImGui settings UI
    virtual void RenderSettings() {}

    void Toggle(JNIEnv* env) {
        m_enabled = !m_enabled;
        if (m_enabled) OnEnable(env);
        else OnDisable(env);
    }

    void SetEnabled(bool enabled, JNIEnv* env) {
        if (m_enabled == enabled) return;
        m_enabled = enabled;
        if (m_enabled) OnEnable(env);
        else OnDisable(env);
    }

    // Getters
    const std::string& GetName() const { return m_name; }
    const std::string& GetDescription() const { return m_description; }
    ModuleCategory GetCategory() const { return m_category; }
    int GetKeybind() const { return m_keybind; }
    void SetKeybind(int key) { m_keybind = key; }
    bool IsEnabled() const { return m_enabled; }
    bool* GetEnabledPtr() { return &m_enabled; }
};
