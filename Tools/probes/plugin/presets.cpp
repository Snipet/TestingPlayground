// PROBE name=plugin.presets timeout=120
//
// plugin.presets: the presets (plugin/PresetStore.h) through the processor's EditorPort, in a folder of the probe's
// own (and never the user's: under CTest the default folder is the probe's sandbox):
//   NAMES    a name tidied: spaces collapsed and trimmed, a slash, a colon, a quote and a leading dot gone, cut to 40;
//            nothing left is ""
//   FOLDER   under CTest the default folder is the sandbox's presets/
//   SAVE     a processor's two files (the second tab shown) and its dials, saved: listed by name with a date; the file
//            is JSON that names this plugin and its kind; presetName() is it, presetEdited() false, then true after an
//            edit
//   LOAD     a fresh processor loads it: the draft is its files and tab (draftRevision bumps), the program runs, and
//            the dials have the saved values; presetEdited() false
//   BYNAME   a preset whose dials are listed in another order, with a name the program lacks: each lands on its own
//            parameter, the unknown is ignored
//   SAME     saving "glass bloom" over "Glass Bloom" replaces it: one preset, in the new name's case
//   SESSION  the session keeps the preset's name
//   REFUSE   a preset of the other kind is refused (the why names it), a damaged file and a missing name too, and the
//            draft is untouched
//   DELETE   deleted, it is gone from the list, and presetName() is "" when it was the one
#include "Probe.h"

#include "plugin/EditorPort.h"
#include "plugin/PresetStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace tp = testingplaygroundsynth;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;

    // A program of two files: its two dials, level and tone, and a function from gain.fm.
    tp::Sources twoFiles()
    {
        const std::string params = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float tone [[fminus::range(100, 10000)]] [[fminus::default(1000)]];\n";
        const std::string program = kInstrument
            ? "#include \"gain.fm\"\nmodule Voice\n{\n" + params
                  + "    bool held;\n    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] += half(level[i]); right[i] += half(level[i]); }\n"
                    "        return held;\n    }\n};\n"
            : "#include \"gain.fm\"\nmodule Effect\n{\n" + params
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] = half(left[i]) * level[i]; right[i] = half(right[i]) * level[i]; }\n"
                    "    }\n};\n";
        return { { tp::kProgramName, program }, { "gain.fm", "float half(float x) { return 0.5f * x; }\n" } };
    }

    // A dial's value in the program's units.
    float dial(tp::Processor& p, int slot)
    {
        return tp::plainOf(p.control(slot), p.getParameters()[slot]->getValue());
    }

    void setDial(tp::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(tp::normOf(p.control(slot), plain));
    }

    bool listed(tp::EditorPort& port, const std::string& name, std::string* when = nullptr)
    {
        for (const tp::PresetEntry& e : port.presets())
            if (e.name == name)
            {
                if (when != nullptr)
                    *when = e.when;
                return true;
            }
        return false;
    }
}

PROBE(plugin, presets)
{
    const funkgui::HeadlessGuiScope gui;

    // FOLDER: the sandbox under CTest (the probe is never run outside it).
    const std::string sandbox = std::string(tp::product::kEnvPrefix) + "PREFS_DIR";
    const char* prefs = std::getenv(sandbox.c_str());
    P.eq("folder.sandbox", prefs != nullptr && *prefs != '\0'
                               && tp::presetstore::directory() == juce::File(juce::String::fromUTF8(prefs)).getChildFile("presets")
                               ? 1 : 0, 1);
    // A folder of the probe's own for the rest.
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("presets-probe-" + juce::String(juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    tp::presetstore::setDirectoryForTests(dir);

    // NAMES
    P.eq("names.tidy", tp::presetstore::tidyName("  Glass   Bloom / 2: \"x\"  ") == "Glass Bloom 2 x" ? 1 : 0, 1);
    P.eq("names.leading_dot", tp::presetstore::tidyName("..hidden") == "hidden" ? 1 : 0, 1);
    P.eq("names.cut", tp::presetstore::tidyName(std::string(60, 'a')).size() == tp::presetstore::kMaxName ? 1 : 0, 1);
    P.eq("names.nothing_left", tp::presetstore::tidyName(" /:: ").empty() ? 1 : 0, 1);

    // SAVE
    const tp::Sources files = twoFiles();
    tp::Processor a;
    tp::EditorPort& pa = a.editorPort();
    pa.setDraft({ files, 1 });
    pa.run(files);
    a.settle();
    P.eq("save.program_runs", pa.status().ok && pa.running() == files ? 1 : 0, 1);
    setDial(a, 0, 0.8f);
    setDial(a, 1, 2500.0f);
    std::string why;
    P.eq("save.saved", pa.savePreset("Glass Bloom", why) ? 1 : 0, 1);
    if (!why.empty())
        std::printf("save: %s\n", why.c_str());
    std::string when;
    P.eq("save.listed", listed(pa, "Glass Bloom", &when) && pa.presets().size() == 1 ? 1 : 0, 1);
    std::printf("listed: Glass Bloom, %s\n", when.c_str());
    P.eq("save.dated", when.size() == 16 && when[4] == '-' && when[13] == ':' ? 1 : 0, 1);
    const juce::File file = dir.getChildFile("Glass Bloom.fmpreset");
    const juce::var json = juce::JSON::parse(file.loadFileAsString());
    P.eq("save.json", json["format"].toString() == "funk-playground-preset" && json["plugin"].toString() == tp::product::kName
                          && json["kind"].toString() == tp::product::kKind ? 1 : 0, 1);
    P.eq("save.name_now", pa.presetName() == "Glass Bloom" && !pa.presetEdited() ? 1 : 0, 1);
    tp::Draft edited = pa.draft();
    edited.files[0].text += "\n// an edit\n";
    pa.setDraft(edited);
    P.eq("save.edit_noticed", pa.presetEdited() ? 1 : 0, 1);

    // LOAD
    tp::Processor b;
    tp::EditorPort& pb = b.editorPort();
    const uint64_t before = pb.draftRevision();
    P.eq("load.loaded", pb.loadPreset("Glass Bloom", why) ? 1 : 0, 1);
    P.eq("load.draft", pb.draft().files == files && pb.draft().active == 1 && pb.draftRevision() > before ? 1 : 0, 1);
    b.settle();
    P.eq("load.runs", pb.status().ok && pb.running() == files ? 1 : 0, 1);
    std::printf("loaded dials: level %.4f, tone %.2f\n", static_cast<double>(dial(b, 0)), static_cast<double>(dial(b, 1)));
    P.near("load.dial_level", static_cast<double>(dial(b, 0)), 0.8, 1e-4);
    P.near("load.dial_tone", static_cast<double>(dial(b, 1)), 2500.0, 0.05);
    P.eq("load.not_edited", pb.presetName() == "Glass Bloom" && !pb.presetEdited() ? 1 : 0, 1);

    // BYNAME: the dials listed tone first, and one the program lacks.
    {
        juce::var root = juce::JSON::parse(file.loadFileAsString());
        auto* d1 = new juce::DynamicObject();
        d1->setProperty("name", "tone");
        d1->setProperty("value", 4000.0);
        auto* d2 = new juce::DynamicObject();
        d2->setProperty("name", "nosuch");
        d2->setProperty("value", 7.0);
        auto* d3 = new juce::DynamicObject();
        d3->setProperty("name", "level");
        d3->setProperty("value", 0.25);
        root.getDynamicObject()->setProperty("dials", juce::Array<juce::var>{ juce::var(d1), juce::var(d2), juce::var(d3) });
        dir.getChildFile("Reordered.fmpreset").replaceWithText(juce::JSON::toString(root));
        tp::Processor c;
        P.eq("byname.loaded", c.editorPort().loadPreset("Reordered", why) ? 1 : 0, 1);
        c.settle();
        P.near("byname.level", static_cast<double>(dial(c, 0)), 0.25, 1e-4);
        P.near("byname.tone", static_cast<double>(dial(c, 1)), 4000.0, 0.05);
    }

    // SAME: one name, case aside.
    P.eq("same.saved", pa.savePreset("glass bloom", why) ? 1 : 0, 1);
    P.eq("same.replaced", listed(pa, "glass bloom") && !listed(pa, "Glass Bloom") && pa.presets().size() == 2 ? 1 : 0, 1);

    // SESSION
    {
        juce::MemoryBlock state;
        a.getStateInformation(state);
        tp::Processor d;
        d.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("session.keeps_the_name", d.editorPort().presetName() == "glass bloom" ? 1 : 0, 1);
    }

    // REFUSE
    {
        juce::var root = juce::JSON::parse(dir.getChildFile("glass bloom.fmpreset").loadFileAsString());
        root.getDynamicObject()->setProperty("kind", kInstrument ? "effect" : "instrument");
        root.getDynamicObject()->setProperty("plugin", "OtherPlayground");
        dir.getChildFile("Other Kind.fmpreset").replaceWithText(juce::JSON::toString(root));
        dir.getChildFile("Broken.fmpreset").replaceWithText("{ not json");
        tp::Processor e;
        const tp::Draft untouched = e.editorPort().draft();
        why.clear();
        P.eq("refuse.other_kind", !e.editorPort().loadPreset("Other Kind", why) && why.find(kInstrument ? "effect" : "instrument")
                                                                                  != std::string::npos ? 1 : 0, 1);
        std::printf("other kind: %s\n", why.c_str());
        P.eq("refuse.damaged", !e.editorPort().loadPreset("Broken", why) && why.find("damaged") != std::string::npos ? 1 : 0, 1);
        P.eq("refuse.missing", !e.editorPort().loadPreset("Nothing Here", why) ? 1 : 0, 1);
        P.eq("refuse.draft_untouched", e.editorPort().draft() == untouched ? 1 : 0, 1);
    }

    // DELETE
    P.eq("delete.deleted", pa.deletePreset("glass bloom", why) ? 1 : 0, 1);
    P.eq("delete.gone", !listed(pa, "glass bloom") ? 1 : 0, 1);
    P.eq("delete.name_cleared", pa.presetName().empty() ? 1 : 0, 1);

    tp::presetstore::setDirectoryForTests({});
    dir.deleteRecursively();
    return P.finish();
}
