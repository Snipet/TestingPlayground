// Source/editor/ParamModel.cpp: see ParamModel.h.
#include "editor/ParamModel.h"

#include <funkgui/core/Format.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace testingplayground::ui
{
    ParamModel::ParamModel(const ParamSpec& spec, funkgui::ParamPort& port) : spec_(spec), port_(port) {}

    uint64_t ParamModel::key() const
    {
        return std::bit_cast<uint32_t>(port_.value01());
    }

    float ParamModel::host01FromTrack(float t) const
    {
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    void ParamModel::view(funkgui::ValueView& v) const
    {
        const float h = port_.value01();
        v = funkgui::ValueView{};
        v.state = funkgui::ValueState::continuous;
        v.label = spec_.label;
        v.track = h;
        v.trackDefault = port_.default01();
        v.bipolar = isBipolar(spec_);
        v.atDefault = std::fabs(h - v.trackDefault) < 1e-6f;
        format(spec_, fromNormalised(spec_, h), v.text);
    }

    void ParamModel::format(const ParamSpec& spec, float plain, funkgui::ValueText& out)
    {
        out = funkgui::ValueText{};
        const char* unit = "";
        const char* spokenUnit = "";
        switch (spec.unit)
        {
            case Unit::db:
                funkgui::fmt::db(plain, 1, out.value, sizeof out.value);
                unit = "DB";
                spokenUnit = "decibels";
                break;
            case Unit::hz:
                if (plain < 99.95f)                      // fmt::hz prints whole hertz: an LFO rate needs decimals
                {
                    std::snprintf(out.value, sizeof out.value, plain < 9.995f ? "%.2f" : "%.1f",
                                  static_cast<double>(plain));
                    unit = "HZ";
                    spokenUnit = "hertz";
                    break;
                }
                funkgui::fmt::hz(plain, out.value, sizeof out.value, &unit);
                if (std::strcmp(unit, "K") == 0)
                {
                    unit = "KHZ";
                    spokenUnit = "kilohertz";
                }
                else
                {
                    spokenUnit = "hertz";
                }
                break;
            case Unit::ms:
                funkgui::fmt::seconds(plain / 1000.0f, out.value, sizeof out.value, &unit);
                spokenUnit = std::strcmp(unit, "S") == 0 ? "seconds" : (std::strcmp(unit, "MS") == 0 ? "milliseconds"
                                                                                                    : "microseconds");
                break;
            case Unit::percent:
                funkgui::fmt::percent(plain / 100.0f, out.value, sizeof out.value);
                unit = "%";
                spokenUnit = "percent";
                break;
            case Unit::m:
                std::snprintf(out.value, sizeof out.value, plain < 9.95f ? "%.1f" : "%.0f", static_cast<double>(plain));
                unit = "M";
                spokenUnit = "metres";
                break;
            case Unit::none:
                std::snprintf(out.value, sizeof out.value, "%.2f", static_cast<double>(plain));
                break;
        }
        std::snprintf(out.unit, sizeof out.unit, "%s", unit);
        std::snprintf(out.spoken, sizeof out.spoken, "%s%s%s", out.value, spokenUnit[0] != '\0' ? " " : "",
                      spokenUnit);
    }
}
