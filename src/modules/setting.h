#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cmath>

// =================================================================
// Setting
// =================================================================
// A module used to do three separate jobs for every value it owns:
// declare the field, register it with the config system by name,
// and then hand-draw a control for it with whatever ImGui widget
// was nearest. The three drifted apart immediately. Ranges lived
// in the draw call, so the config could write a number the slider
// could never produce. Half the values were never drawn at all.
// And because every module drew its own panel, the client had
// fifteen slightly different ideas of what a slider is.
//
// A setting now DESCRIBES itself once, in the constructor, and
// everything else is derived from that description:
//
//   the panel        drawn generically, so every control in the
//                    client behaves identically
//   the config       saved and restored by name, clamped on load
//   search           settings are searchable, not just modules
//   reset            a default is captured at bind time
//
// No module draws its own settings any more. If a control cannot
// be expressed here, that is an argument for extending this file,
// not for going around it.
// =================================================================

struct Setting {
    enum class Type { Bool, Int, Float, Enum };

    // ---- Identity ----
    // `key` is what the config file stores. It never changes once
    // shipped, or every existing profile silently loses the value.
    // `label` is what the user reads and may be reworded freely.
    std::string key;
    std::string label;
    std::string hint;      // one quiet line under the control
    std::string group;     // optional heading inside the panel

    Type  type = Type::Bool;
    void* ptr  = nullptr;

    // ---- Numeric range ----
    // Authoritative. The panel cannot exceed it and the config
    // loader clamps to it, so a hand-edited file can no longer put
    // a module into a state it has no way back out of.
    float min  = 0.0f;
    float max  = 1.0f;
    float step = 0.0f;              // 0 = continuous
    std::string format = "%.2f";

    // ---- Enum ----
    std::vector<std::string> options;
    std::vector<std::string> optionHints;

    // ---- Presentation ----
    bool advanced = false;          // lives behind the disclosure

    // Settings that only make sense in some modes hide themselves
    // rather than sitting there greyed out and confusing. Evaluated
    // on the render thread, reading only the module's own fields.
    std::function<bool()> visible;

    // ---- Default, captured from the member initialiser ----
    float defaultValue = 0.0f;

    // -------------------------------------------------------------
    // Fluent description
    // -------------------------------------------------------------
    Setting& Label(const char* v)  { label = v ? v : ""; return *this; }
    Setting& Hint(const char* v)   { hint  = v ? v : ""; return *this; }
    Setting& Group(const char* v)  { group = v ? v : ""; return *this; }
    Setting& Advanced(bool v = true) { advanced = v; return *this; }

    Setting& Range(float lo, float hi) {
        min = lo; max = hi;
        return *this;
    }
    Setting& Step(float v)   { step = v; return *this; }
    Setting& Format(const char* v) { if (v) format = v; return *this; }

    // Percentages, ticks and degrees are the three units this client
    // actually uses, and spelling them out at every call site is how
    // "85" ends up meaning blocks in one panel and percent in the
    // next.
    Setting& Percent() { format = "%.0f%%"; return *this; }
    Setting& Ticks()   { format = "%.0f t"; return *this; }
    Setting& Degrees() { format = "%.0f\xC2\xB0"; return *this; }
    Setting& Blocks()  { format = "%.2f b"; return *this; }
    Setting& Ms()      { format = "%.0f ms"; return *this; }

    Setting& Options(std::initializer_list<const char*> v) {
        options.clear();
        for (const char* s : v) options.push_back(s ? s : "");
        max = options.empty() ? 0.0f : (float)(options.size() - 1);
        min = 0.0f;
        return *this;
    }

    // One line of explanation per mode, shown beside the option in
    // the picker. A mode list without these is a quiz.
    Setting& OptionHints(std::initializer_list<const char*> v) {
        optionHints.clear();
        for (const char* s : v) optionHints.push_back(s ? s : "");
        return *this;
    }

    Setting& VisibleIf(std::function<bool()> fn) {
        visible = std::move(fn);
        return *this;
    }

    // -------------------------------------------------------------
    // Access
    // -------------------------------------------------------------
    bool IsVisible() const { return !visible || visible(); }

    bool  AsBool()  const { return ptr ? *(bool*)ptr : false; }
    int   AsInt()   const { return ptr ? *(int*)ptr : 0; }
    float AsFloat() const { return ptr ? *(float*)ptr : 0.0f; }

    // One numeric view of any setting, which is what the config and
    // the search index want. Booleans are 0 or 1, enums are their
    // index.
    float Get() const {
        switch (type) {
            case Type::Bool:  return AsBool() ? 1.0f : 0.0f;
            case Type::Int:   return (float)AsInt();
            case Type::Enum:  return (float)AsInt();
            default:          return AsFloat();
        }
    }

    float Clamp(float v) const {
        if (v < min) v = min;
        if (v > max) v = max;
        if (step > 0.0f) {
            float n = std::floor((v - min) / step + 0.5f);
            v = min + n * step;
            if (v > max) v = max;
        }
        return v;
    }

    // Writes through the range, always. Every path into a setting
    // goes here: the panel, the config loader and reset-to-default.
    // There is deliberately no way to store an out-of-range value.
    bool Set(float v) {
        if (!ptr) return false;
        switch (type) {
            case Type::Bool: {
                bool nv = (v != 0.0f);
                if (*(bool*)ptr == nv) return false;
                *(bool*)ptr = nv;
                return true;
            }
            case Type::Int:
            case Type::Enum: {
                float c = Clamp(v);
                int nv = (int)std::floor(c + 0.5f);
                if (*(int*)ptr == nv) return false;
                *(int*)ptr = nv;
                return true;
            }
            default: {
                float nv = Clamp(v);
                if (*(float*)ptr == nv) return false;
                *(float*)ptr = nv;
                return true;
            }
        }
    }

    bool IsDefault() const {
        if (type == Type::Float) return std::fabs(Get() - defaultValue) < 1e-4f;
        return Get() == defaultValue;
    }

    bool ResetToDefault() { return Set(defaultValue); }

    // What the value reads as, for the collapsed row and for search
    // results. Enums show their option name, not "3".
    std::string Display() const {
        char buf[64];
        switch (type) {
            case Type::Bool:
                return AsBool() ? "On" : "Off";
            case Type::Enum: {
                int i = AsInt();
                if (i >= 0 && i < (int)options.size()) return options[i];
                return "-";
            }
            default:
                snprintf(buf, sizeof(buf), format.c_str(), Get());
                return buf;
        }
    }

    const std::string& Text() const { return label.empty() ? key : label; }
};
