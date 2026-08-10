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

// =================================================================
// Setting
// =================================================================
// A setting used to be a name and a pointer, and nothing else. The
// panel that showed it was then written out by hand, in raw ImGui,
// once per module. That had three consequences and all three were
// real bugs rather than theory:
//
//   1. The config key and the label on screen were two different
//      strings that had to be kept in step by hand. They drifted,
//      and a preset that named the old one silently did nothing.
//   2. A setting could be bound and never drawn, so it existed in
//      the config file and nowhere in the interface.
//   3. Fifteen hand-written panels meant fifteen slightly different
//      sliders. Some had a typed value, some did not. Some clamped,
//      some did not.
//
// So a setting now carries everything needed to draw itself, and
// there is exactly ONE renderer for all of them. The name is the
// config key AND the label, so they cannot disagree. The hint is
// mandatory in spirit: a control nobody can explain should not be
// exposed at all.
// =================================================================
struct Setting {
    enum class Type { Bool, Int, Float, Mode, Color };

    std::string name;               // config key and on-screen label
    const char* hint = nullptr;     // one line under the control
    Type  type = Type::Bool;
    void* ptr  = nullptr;

    float lo = 0.0f;
    float hi = 1.0f;
    const char* fmt = "%.2f";

    const char* const* options = nullptr;   // Mode only
    int optionCount = 0;

    // Everyday settings are on the panel. Everything else is behind
    // Advanced, which most people will never open and should never
    // need to.
    bool advanced = false;

    // A control that does nothing in the current mode is worse than
    // a missing one, because it invites you to change it and then
    // ignores you. Rows declare which modes they belong to and are
    // simply not drawn otherwise.
    //
    // A MASK rather than a single value, because most settings apply
    // to more than one mode: a jump delay belongs to both Jump Reset
    // and Combined, and pretending otherwise was how the first
    // version of this ended up hiding half of Velocity.
    int      dependOn = -1;         // index of the governing setting
    unsigned dependMask = 0;        // bit per allowed value
    bool     dependNegate = false;

    bool  AsBool()  const { return *(bool*)ptr; }
    int   AsInt()   const { return *(int*)ptr; }
    float AsFloat() const { return *(float*)ptr; }

    // How many scalars this setting is made of. A colour is four
    // (RGBA); everything else is one. Used by the config layer to
    // persist a colour as four keys instead of packing four channels
    // into a single float and losing them to ostream formatting.
    int Components() const { return type == Type::Color ? 4 : 1; }

    // The value a config file stores, whatever the type underneath.
    float Read() const {
        switch (type) {
            case Type::Bool:  return *(bool*)ptr ? 1.0f : 0.0f;
            case Type::Int:
            case Type::Mode:  return (float)*(int*)ptr;
            case Type::Color: return ((const float*)ptr)[0];
            default:          return *(float*)ptr;
        }
    }

    // One channel of a colour, or the scalar value for everything
    // else. i is 0..3 for RGBA.
    float ReadComp(int i) const {
        if (type == Type::Color) {
            const float* c = (const float*)ptr;
            return (i >= 0 && i < 4) ? c[i] : 0.0f;
        }
        return Read();
    }

    // Clamped on the way in, so a hand-edited or outdated config
    // cannot push a module into a state its own interface can never
    // produce.
    void Write(float v) {
        switch (type) {
            case Type::Bool:
                *(bool*)ptr = (v != 0.0f);
                break;
            case Type::Mode: {
                int i = (int)v;
                if (i < 0) i = 0;
                if (optionCount > 0 && i >= optionCount) i = optionCount - 1;
                *(int*)ptr = i;
                break;
            }
            case Type::Int: {
                float c = v < lo ? lo : (v > hi ? hi : v);
                *(int*)ptr = (int)(c + (c < 0.0f ? -0.5f : 0.5f));
                break;
            }
            case Type::Color: {
                // The scalar path only ever reaches the first
                // channel; the loader uses WriteComp for the rest.
                float* c = (float*)ptr;
                c[0] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                break;
            }
            default:
                *(float*)ptr = v < lo ? lo : (v > hi ? hi : v);
                break;
        }
    }

    // One channel of a colour, clamped to 0..1. Anything else falls
    // through to the scalar Write.
    void WriteComp(int i, float v) {
        if (type == Type::Color) {
            if (i < 0 || i > 3) return;
            ((float*)ptr)[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return;
        }
        Write(v);
    }
};

// Small fluent handle returned by Bind. It holds an index rather
// than a reference because push_back moves the vector out from
// under anything holding a pointer into it.
struct SettingRef {
    std::vector<Setting>* vec = nullptr;
    size_t index = 0;

    Setting& S() const { return (*vec)[index]; }

    SettingRef& Hint(const char* h)   { S().hint = h; return *this; }
    SettingRef& Advanced()            { S().advanced = true; return *this; }
    SettingRef& Format(const char* f) { S().fmt = f; return *this; }

    // Draw this row only while another setting holds one of the
    // given values. Bits, so several modes can share a control.
    SettingRef& When(const char* other, int value) {
        return Depend(other, Bit(value), false);
    }
    SettingRef& WhenAny(const char* other, int a, int b, int c = -1) {
        return Depend(other, Bit(a) | Bit(b) | Bit(c), false);
    }
    SettingRef& Unless(const char* other, int value) {
        return Depend(other, Bit(value), true);
    }
    SettingRef& UnlessAny(const char* other, int a, int b, int c = -1) {
        return Depend(other, Bit(a) | Bit(b) | Bit(c), true);
    }

private:
    static unsigned Bit(int v) {
        return (v >= 0 && v < 32) ? (1u << v) : 0u;
    }

    SettingRef& Depend(const char* other, unsigned mask, bool negate) {
        for (size_t i = 0; i < vec->size(); i++) {
            if ((*vec)[i].name != other) continue;
            S().dependOn     = (int)i;
            S().dependMask   = mask;
            S().dependNegate = negate;
            break;
        }
        return *this;
    }
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

    SettingRef Push(Setting s) {
        m_settings.push_back(s);
        return SettingRef{ &m_settings, m_settings.size() - 1 };
    }

    SettingRef Bind(const char* name, bool* p, const char* hint = nullptr) {
        Setting s;
        s.name = name; s.hint = hint;
        s.type = Setting::Type::Bool; s.ptr = p;
        return Push(s);
    }

    SettingRef Bind(const char* name, int* p, int lo, int hi,
                    const char* hint = nullptr)
    {
        Setting s;
        s.name = name; s.hint = hint;
        s.type = Setting::Type::Int; s.ptr = p;
        s.lo = (float)lo; s.hi = (float)hi; s.fmt = "%.0f";
        return Push(s);
    }

    SettingRef Bind(const char* name, float* p, float lo, float hi,
                    const char* fmt = "%.2f", const char* hint = nullptr)
    {
        Setting s;
        s.name = name; s.hint = hint;
        s.type = Setting::Type::Float; s.ptr = p;
        s.lo = lo; s.hi = hi; s.fmt = fmt;
        return Push(s);
    }

    // A mode is an int with names. It is drawn as a segmented
    // control and stored as a number, and the number is clamped to
    // the option count on load so removing a mode in a later build
    // cannot leave an old config pointing at nothing.
    SettingRef BindMode(const char* name, int* p,
                        const char* const* options, int count,
                        const char* hint = nullptr)
    {
        Setting s;
        s.name = name; s.hint = hint;
        s.type = Setting::Type::Mode; s.ptr = p;
        s.options = options; s.optionCount = count;
        s.lo = 0.0f; s.hi = (float)(count - 1); s.fmt = "%.0f";
        return Push(s);
    }

    // A colour: four contiguous floats, RGBA, each 0..1. Drawn as a
    // swatch that opens a picker, and persisted as four suffixed
    // keys rather than one packed float, so no precision is lost.
    // The pointer is to the first of the four.
    SettingRef BindColor(const char* name, float* rgba,
                         const char* hint = nullptr)
    {
        Setting s;
        s.name = name; s.hint = hint;
        s.type = Setting::Type::Color; s.ptr = rgba;
        s.lo = 0.0f; s.hi = 1.0f;
        return Push(s);
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

    // -------------------------------------------------------------
    // OnReset
    // -------------------------------------------------------------
    // The world underneath the module changed: a server switch, a
    // dimension change, a death and respawn, or a reconnect.
    //
    // Everything a module remembers about "the situation" is now
    // wrong. Held targets point at entities that no longer exist,
    // timers are counting toward a fight that already ended, and
    // recorded positions belong to another world. Carrying any of
    // that across is how a module comes back from a respawn aiming
    // at nothing or refusing to fire.
    //
    // The module STAYS ENABLED. This is not a disable: it is the
    // module being handed a clean slate and told to carry on.
    // Anything held on the player's behalf must still be let go,
    // because the reset may have happened mid-action.
    //
    // JNI is safe to use here, but the player and world may be null.
    virtual void OnReset(JNIEnv*) {}

    // -------------------------------------------------------------
    // Panel
    // -------------------------------------------------------------
    // Modules no longer draw their own settings. The renderer walks
    // the bound settings and produces the same rows, in the same
    // order, with the same behaviour, for every module in the
    // client. That is the only way "every slider works the same"
    // stays true six modules from now.
    //
    // Notice is the one exception, and it is not a control: it is a
    // line of text the module wants to say about the situation right
    // now, such as a keybind it could not resolve. Returning a
    // severity lets the renderer style it like every other notice
    // rather than each module picking its own shade of red.
    enum class NoticeLevel { None, Info, Warning, Danger };

    virtual NoticeLevel Notice(const char** text) const {
        (void)text;
        return NoticeLevel::None;
    }

    // A one-line live state string for the collapsed row, so the
    // panel does not have to be open to see what a module is doing.
    virtual const char* StatusLine() const { return nullptr; }

    bool HasAdvanced() const {
        for (auto& s : m_settings) if (s.advanced) return true;
        return false;
    }

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
            s.Write(value);
            return true;
        }
        return false;
    }

    // Route one channel of a colour setting, used by the config
    // loader for the suffixed .r/.g/.b/.a keys. Returns false when
    // the name is not a colour, so a scalar key that happens to end
    // in .r still falls through to SetValue rather than being eaten.
    bool SetColorComponent(const std::string& setting, int comp, float value) {
        for (auto& s : m_settings) {
            if (s.name != setting || s.type != Setting::Type::Color) continue;
            s.WriteComp(comp, value);
            return true;
        }
        return false;
    }

    const std::vector<Setting>& GetSettings() const { return m_settings; }
    std::vector<Setting>&       GetSettings()       { return m_settings; }

    const std::string& GetName() const        { return m_name; }
    const std::string& GetDescription() const { return m_description; }
    ModuleCategory     GetCategory() const    { return m_category; }
    int  GetKeybind() const                   { return m_keybind.load(); }
    void SetKeybind(int key)                  { m_keybind.store(key); }
    bool IsEnabled() const                    { return m_enabled.load(); }
};
