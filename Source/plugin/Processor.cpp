// Source/plugin/Processor.cpp: see Processor.h.
#include "plugin/Processor.h"

#include "plugin/ProjectExport.h"
#include "plugin/ProjectStore.h"

#include "Examples.h"
#include "Product.h"

#include "fsdsp/core/Sanitize.h"

#include <funkgui/gpu/ShaderLibrary.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace playground
{
    namespace
    {
        // The program in the plugin state, beside the parameters: <PROGRAM active=".."> with <RUN> (the files that
        // play) and <DRAFT> (the editor's), each of <FILE name=".." text=".."/>s. The version before files saved
        // <PROGRAM source=".." draft=".."/>, one file each.
        const juce::Identifier kProgramTag{ "PROGRAM" }, kRunTag{ "RUN" }, kDraftTag{ "DRAFT" }, kFileTag{ "FILE" };
        const juce::Identifier kActive{ "active" }, kName{ "name" }, kText{ "text" }, kPreset{ "preset" },
                               kProject{ "project" }, kProjectVersion{ "projectVersion" }, kProjectPreset{ "projectPreset" };
        const juce::Identifier kSource{ "source" }, kDraft{ "draft" };   // the version before files
        const juce::Identifier kChatTag{ "CHAT" }, kJson{ "json" };      // the chat with Claude (ChatAgent::saveState)
        const juce::Identifier kEditorTag{ "EDITOR" }, kWidth{ "w" }, kHeight{ "h" };   // the editor's size
        const juce::Identifier kView{ "view" };                                          // and its view: "panel"
        const juce::Identifier kChat{ "chat" };                                          // its chat: "closed"
        const juce::Identifier kExplorer{ "explorer" };                                  // its explorer: "closed"
        const juce::Identifier kPageTag{ "PAGE" }, kTitle{ "title" };                    // its interface's pages
        constexpr int kMaxEditorSide = 16384;

        // What building a .gui file's shaders needs (fminus/Shaders.h): shaderc, bundled in the plugin
        // (Contents/Resources/shaderc: CMakeLists.txt copies the pinned bgfx's there), else where <prefix>SHADERC says
        // (the probes'); FunkGui's prelude, varyings and bgfx_shader.sh; the renderer's target.
        ShaderTools shaderTools()
        {
            ShaderTools t;
            const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
            const juce::File bundled = exe.getParentDirectory().getSiblingFile("Resources").getChildFile("shaderc");
            const char* env = std::getenv((std::string(product::kEnvPrefix) + "SHADERC").c_str());
            if (bundled.existsAsFile())
                t.shaderc = bundled.getFullPathName().toStdString();
            else if (env != nullptr && *env != '\0')
                t.shaderc = env;
            t.prelude = funkgui::shaders::fragmentPrelude();
            t.varyingDef = funkgui::shaders::varyingDef();
            t.bgfxShaderSh = funkgui::shaders::bgfxShaderSh();
#if defined(__APPLE__)
            t.platform = "osx";
            t.profile = "metal";
#else
            t.platform = "linux";
            t.profile = "spirv";
#endif
            return t;
        }
        const juce::Identifier kParamId{ "id" }, kParamValue{ "value" };   // the APVTS's <PARAM id value> children

        constexpr int kMaxChannels = 8;                          // handed to the Engine (the layouts allow two)
        constexpr double kAlertMs = 2000.0;                      // how long an alert stays shown

        juce::String idOf(int c) { return "p" + juce::String(c + 1); }

        juce::ValueTree filesTree(const juce::Identifier& tag, const Sources& files)
        {
            juce::ValueTree tree{ tag };
            for (const SourceFile& f : files)
            {
                juce::ValueTree file{ kFileTag };
                file.setProperty(kName, juce::String::fromUTF8(f.name.c_str()), nullptr);
                file.setProperty(kText, juce::String::fromUTF8(f.text.c_str()), nullptr);
                tree.appendChild(file, nullptr);
            }
            return tree;
        }

        // A <RUN> or <DRAFT>'s files; or, from a session of the version before files, `oldText` as one file.
        Sources filesOf(const juce::ValueTree& tree, const juce::String& oldText)
        {
            Sources files;
            for (const juce::ValueTree& file : tree)
                if (file.hasType(kFileTag))
                    files.push_back({ file.getProperty(kName).toString().toStdString(),
                                      file.getProperty(kText).toString().toStdString() });
            if (files.empty() && oldText.isNotEmpty())
                files = singleFile(oldText.toStdString());
            return tidyFiles(std::move(files));
        }

        // The host's text for slot c's normalised value: the program's plain value, with a dash for a host's NaN.
        juce::String plainText(const Control& c, float v)
        {
            if (!std::isfinite(v))
                return juce::String::fromUTF8("\xE2\x80\x93");
            if (!c.used)
                return juce::String(static_cast<double>(v), 2);
            const float p = plainOf(c, v);
            switch (c.type)
            {
                case ControlType::toggle:  return p >= 0.5f ? "on" : "off";
                case ControlType::integer: return juce::String(static_cast<int>(p));
                case ControlType::real:    break;
            }
            char buf[32];
            formatValue(c, p, buf, sizeof buf);      // its unit, decimals and scale (a .gui file's)
            return juce::String(buf);
        }

        // P1 to P32: a host parameter whose name and text are the running program's (Processor::control). P17 on came
        // with the plugin's second parameter version (the AU's version hint), so a host's automation of the first stays.
        class ControlParameter final : public juce::AudioParameterFloat
        {
        public:
            ControlParameter(int slot, const Processor& owner)
                : juce::AudioParameterFloat(juce::ParameterID{ idOf(slot), slot < kFirstControls ? 1 : 2 }, "P" + juce::String(slot + 1),
                                            juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
                  slot_(slot), owner_(owner) {}

            juce::String getName(int maximumStringLength) const override
            {
                const Control c = owner_.control(slot_);
                juce::String name = juce::String(slot_ + 1);
                if (c.used)
                    name << " " << juce::String(c.name);
                return name.substring(0, maximumStringLength);
            }

            juce::String getText(float v, int maximumStringLength) const override
            {
                const Control c = owner_.control(slot_);
                // An int's or a bool's value by the name the program's panel gives it (#pragma panel options).
                if (c.used && c.type != ControlType::real && std::isfinite(v))
                {
                    const std::vector<std::string> names = owner_.controlOptions(slot_);
                    const long k = std::lround(plainOf(c, v)) - std::lround(c.min);
                    if (k >= 0 && static_cast<std::size_t>(k) < names.size() && !names[static_cast<std::size_t>(k)].empty())
                        return juce::String(names[static_cast<std::size_t>(k)]).substring(0, maximumStringLength);
                }
                return plainText(c, v).substring(0, maximumStringLength);
            }

            float getValueForText(const juce::String& text) const override
            {
                const Control c = owner_.control(slot_);
                const juce::String t = text.trim().toLowerCase();
                if (!c.used)
                    return std::clamp(t.getFloatValue(), 0.0f, 1.0f);
                const std::vector<std::string> names = owner_.controlOptions(slot_);
                for (std::size_t k = 0; k < names.size(); ++k)
                    if (!names[k].empty() && t == juce::String(names[k]).toLowerCase())
                        return normOf(c, c.min + static_cast<float>(k));
                if (c.type == ControlType::toggle)
                    return t == "on" || t == "yes" || t == "true" || t.getFloatValue() >= 0.5f ? 1.0f : 0.0f;
                if (c.type == ControlType::integer)
                    return normOf(c, t.getFloatValue());
                const float plain = parseValue(c, t.toRawUTF8());
                return std::isfinite(plain) ? normOf(c, plain) : normOf(c, c.def);
            }

        private:
            int slot_;
            const Processor& owner_;
        };

        // The Event of a MIDI message an instrument plays, else false: note-on (at velocity 0, a note-off), note-off,
        // and the controllers All Sound Off (120) and All Notes Off (123), both allNotesOff. The caller sets the
        // offset. (An effect never calls it: maybe_unused.)
        [[maybe_unused]] bool toEvent(const juce::MidiMessageMetadata& m, Event& e) noexcept
        {
            if (m.numBytes < 1)
                return false;
            const int status = m.data[0] & 0xf0;
            const uint8_t d1 = m.numBytes > 1 ? m.data[1] : 0;
            const uint8_t d2 = m.numBytes > 2 ? m.data[2] : 0;
            e.note = d1;
            if (status == 0x90 && d2 > 0)
            {
                e.kind = EventKind::noteOn;
                e.velocity = static_cast<float>(d2) / 127.0f;
            }
            else if (status == 0x80 || status == 0x90)
                e.kind = EventKind::noteOff;
            else if (status == 0xb0 && (d1 == 120 || d1 == 123))
                e.kind = EventKind::allNotesOff;
            else if (status == 0xb0 && (d1 == 1 || d1 == 64))  // the mod wheel, the sustain pedal
            {
                e.kind = EventKind::controller;
                e.velocity = static_cast<float>(d2) / 127.0f;
            }
            else if (status == 0xe0)                            // the pitch wheel: 14 bits, 8192 at rest
            {
                e.kind = EventKind::pitchBend;
                const int v = (static_cast<int>(d2) << 7) | static_cast<int>(d1);
                e.velocity = std::clamp(static_cast<float>(v - 8192) / 8191.0f, -1.0f, 1.0f);
                e.note = 0;
            }
            else if (status == 0xd0)                            // channel pressure
            {
                e.kind = EventKind::pressure;
                e.velocity = static_cast<float>(d1) / 127.0f;
                e.note = 0;
            }
            else
                return false;
            return true;
        }
    }

    juce::AudioProcessor::BusesProperties Processor::buses()
    {
        const auto stereo = juce::AudioChannelSet::stereo();
        if constexpr (product::kIsInstrument)
            return BusesProperties().withOutput("Output", stereo, true);
        else
            return BusesProperties().withInput("Input", stereo, true).withOutput("Output", stereo, true);
    }

    juce::AudioProcessorValueTreeState::ParameterLayout Processor::createLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
        for (int c = 0; c < kMaxControls; ++c)
            params.push_back(std::make_unique<ControlParameter>(c, *this));
        return { params.begin(), params.end() };
    }

    Processor::Processor()
        : juce::AudioProcessor(buses()),
          apvts_(*this, nullptr, "PARAMETERS", createLayout())
    {
        for (int c = 0; c < kMaxControls; ++c)
        {
            const auto i = static_cast<std::size_t>(c);
            raw_[i] = apvts_.getRawParameterValue(idOf(c));
            params_[i] = apvts_.getParameter(idOf(c));
            jassert(raw_[i] != nullptr && params_[i] != nullptr);
            portStore_.push_back(std::make_unique<GesturePort>(*params_[i]));
            ports_.push_back(portStore_.back().get());
        }

        // The first example, Init, plays from the start (fminus.examples and fminus.init check it), its dials at their
        // defaults.
        draft_.files = singleFile(kExamples[0].source);
        projectFiles_ = draft_.files;                            // nothing to lose yet: no project's open asks first
        compiler_.setShaderTools(shaderTools());
        capture_.setOwnCode([this](bool on) { engine_.setOwnCode(on); });
        const CompileStatus s = compiler_.compileNow(draft_.files);
        jassert(s.ok);
        seen_ = s.revision;
        apply(s, true);
        syncDials();                                             // the journal's dials start here
        startTimerHz(20);
        mcp_ = std::make_unique<McpLink>(McpLink::Hooks{ [this] { return mcpInfo(); },
                                                         [this](const std::string& name, const juce::var& arguments, uint64_t call) {
                                                             return chat_.mcpCall(name, arguments, call);
                                                         },
                                                         [this](uint64_t call) { chat_.mcpCancel(call); } });
    }

    void Processor::updateTrackProperties(const TrackProperties& properties)
    {
        const std::lock_guard<std::mutex> lock(trackMutex_);
        trackName_ = properties.name.toStdString();
    }

    // What list_instances says of this one (the MCP socket's thread).
    juce::var Processor::mcpInfo() const
    {
        auto* o = new juce::DynamicObject();
        const juce::var out(o);
        o->setProperty("kind", product::kIsInstrument ? "instrument" : "effect");
        o->setProperty("plugin", product::kName);
        o->setProperty("host", juce::PluginHostType().getHostDescription());
        {
            const std::lock_guard<std::mutex> lock(trackMutex_);
            o->setProperty("track", juce::String::fromUTF8(trackName_.c_str()));
        }
        juce::Array<juce::var> files;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            o->setProperty("project", juce::String::fromUTF8(projectName_.c_str()));
            o->setProperty("version", projectVersion_);
            o->setProperty("preset", juce::String::fromUTF8(projectPreset_.c_str()));
            o->setProperty("edited", !projectName_.empty() && draft_.files != projectFiles_);
            for (const SourceFile& f : draft_.files)
                files.add(juce::String::fromUTF8(f.name.c_str()));
        }
        o->setProperty("files", files);
        o->setProperty("compiled", compiler_.status().ok);
        o->setProperty("busy", chat_.chatState().busy);
        return out;
    }

    Processor::~Processor()
    {
        mcp_.reset();                                            // no call comes in while the rest goes
        stopTimer();
    }

    const juce::String Processor::getName() const { return product::kName; }
    bool Processor::acceptsMidi() const { return product::kIsInstrument; }
    bool Processor::producesMidi() const { return false; }
    bool Processor::isMidiEffect() const { return false; }
    double Processor::getTailLengthSeconds() const { return engine_.tailSeconds(); }

    bool Processor::isBusesLayoutSupported(const BusesLayout& layouts) const
    {
        const juce::AudioChannelSet& in = layouts.getMainInputChannelSet();
        const juce::AudioChannelSet& out = layouts.getMainOutputChannelSet();
        const bool monoOrStereo = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
        if constexpr (product::kIsInstrument)
            return monoOrStereo && in.isDisabled();
        else
            return monoOrStereo && in == out;
    }

    void Processor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
    {
        journalPrepare(sampleRate, std::max(1, maximumExpectedSamplesPerBlock));
        maxBlock_ = std::max(1, maximumExpectedSamplesPerBlock);
        largestCall_ = 0;
        engine_.prepare(sampleRate, maxBlock_);
        capture_.prepare(engine_.rate(), maxBlock_);             // a recording under way ends here
        wet_.prepare(engine_.rate());                            // fsdsp::kFadeMs, 20 ms
        wetSet_ = false;
        weight_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
        if constexpr (!product::kIsInstrument)
            dry_.assign(static_cast<std::size_t>(kMaxChannels) * static_cast<std::size_t>(maxBlock_), 0.0f);
        for (int c = 0; c < kMaxControls; ++c)                   // the first block starts on the dials as they are
            values_[static_cast<std::size_t>(c)] = std::clamp(raw_[static_cast<std::size_t>(c)]->load(), 0.0f, 1.0f);
        prepared_ = true;
    }

    void Processor::releaseResources() {}

    // ---- the audio thread --------------------------------------------------------------------------------------------
    void Processor::readParameters() noexcept
    {
        // A host can write a NaN (JUCE's ranges pass it through): such a value keeps the last good one.
        for (std::size_t c = 0; c < raw_.size(); ++c)
            if (const float v = raw_[c]->load(std::memory_order_relaxed); std::isfinite(v))
                values_[c] = std::clamp(v, 0.0f, 1.0f);
        engine_.setControls(values_);
    }

    void Processor::play(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept
    {
        const int n = buffer.getNumSamples();
        const int numOut = std::min(getTotalNumOutputChannels(), buffer.getNumChannels());
        for (int ch = getTotalNumInputChannels(); ch < numOut; ++ch)
            buffer.clear(ch, 0, n);
        const int numCh = std::min(numOut, kMaxChannels);         // any further channel stays as it is

        // The Engine from `done` to `end` (at most maxBlock_ samples) with the events collected so far, their offsets
        // counted from `done`. While processing (w steady at 1) the output is the Engine's, untouched; otherwise the
        // bypass fade, or the bypassed sound, follows.
        std::size_t numEvents = 0;
        int done = 0;
        const auto playTo = [&](int end) {
            const int len = end - done;
            std::array<float*, kMaxChannels> channels{};
            for (int c = 0; c < numCh; ++c)
                channels[static_cast<std::size_t>(c)] = buffer.getWritePointer(c) + done;
            const bool engineOnly = !wet_.moving() && wet_.lin.cur >= 1.0f;
            if (!engineOnly)
                keepDry(channels.data(), numCh, len);
            largestCall_ = std::max(largestCall_, len);
            capture_.input(channels.data(), numCh, len);
            engine_.process(channels.data(), numCh, len, std::span<const Event>(events_.data(), numEvents));
            capture_.output(channels.data(), numCh, len);
            if (!engineOnly)
                bypassMix(channels.data(), numCh, len);
            numEvents = 0;
            done = end;
        };

        if constexpr (product::kIsInstrument)
        {
            // The panel's keyboard first, at the block's start (at most kUiNotes: events_ holds far more).
            const uint32_t head = uiNoteHead_.load(std::memory_order_acquire);
            uint32_t tail = uiNoteTail_.load(std::memory_order_relaxed);
            for (; tail != head; ++tail)
            {
                const Event e = uiNotes_[tail % kUiNotes];
                if (bypassed && e.kind == EventKind::noteOn)
                    continue;
                capture_.event(0, e);
                events_[numEvents++] = e;
            }
            uiNoteTail_.store(tail, std::memory_order_release);
            for (const juce::MidiMessageMetadata m : midi)
            {
                Event e;
                if (!toEvent(m, e) || (bypassed && e.kind == EventKind::noteOn))
                    continue;
                const int at = std::clamp(m.samplePosition, done, std::max(done, n - 1));
                while (at - done >= maxBlock_)                    // whole parts before the event's
                    playTo(done + maxBlock_);
                if (numEvents == events_.size())                // full: play up to this event, then carry on
                    playTo(at);
                e.offset = static_cast<uint32_t>(at - done);
                capture_.event(at, e);
                events_[numEvents++] = e;
            }
        }
        else
        {
            juce::ignoreUnused(midi, bypassed);
        }
        while (n - done > maxBlock_)
            playTo(done + maxBlock_);
        playTo(n);                                               // also a block of 0 samples: its events apply
    }

    std::vector<std::string> Processor::guiPages() const
    {
        const std::lock_guard<std::mutex> lock(pagesMutex_);
        return guiPages_;
    }

    void Processor::setGuiPages(const std::vector<std::string>& titles)
    {
        const std::lock_guard<std::mutex> lock(pagesMutex_);
        guiPages_ = titles;
    }

    void Processor::setWindowHooks(WindowHooks hooks) { windowHooks_ = std::move(hooks); }

    void Processor::placeWindowButtons(float x, float middle)
    {
        if (windowHooks_.place)
            windowHooks_.place(x, middle);
    }

    void Processor::dragWindow(int clicks)
    {
        if (windowHooks_.drag)
            windowHooks_.drag(clicks);
    }

    void Processor::pushUiEvent(const Event& e)
    {
        const uint32_t head = uiNoteHead_.load(std::memory_order_relaxed);
        if (head - uiNoteTail_.load(std::memory_order_acquire) >= kUiNotes)
            return;                                              // full: dropped
        uiNotes_[head % kUiNotes] = e;
        uiNoteHead_.store(head + 1, std::memory_order_release);
    }

    void Processor::playNote(int note, float velocity)
    {
        if constexpr (product::kIsInstrument)
        {
            if (note < 0 || note > 127 || !std::isfinite(velocity))
                return;
            Event e;
            e.note = static_cast<uint8_t>(note);
            e.velocity = std::clamp(velocity, 0.0f, 1.0f);
            e.kind = e.velocity > 0.0f ? EventKind::noteOn : EventKind::noteOff;
            pushUiEvent(e);
        }
        else
            juce::ignoreUnused(note, velocity);
    }

    void Processor::playWheel(bool pitch, float value)
    {
        if constexpr (product::kIsInstrument)
        {
            if (!std::isfinite(value))
                return;
            Event e;
            if (pitch)
            {
                e.kind = EventKind::pitchBend;
                e.velocity = std::clamp(value, -1.0f, 1.0f);
            }
            else
            {
                e.kind = EventKind::controller;
                e.note = 1;
                e.velocity = std::clamp(value, 0.0f, 1.0f);
            }
            pushUiEvent(e);
        }
        else
            juce::ignoreUnused(pitch, value);
    }

    void Processor::keepDry(float* const* channels, int numChannels, int numSamples) noexcept
    {
        if constexpr (!product::kIsInstrument)
            for (int c = 0; c < numChannels; ++c)
                fsdsp::sanitize(channels[c], dry_.data() + static_cast<std::size_t>(c * maxBlock_), numSamples);
        else
            juce::ignoreUnused(channels, numChannels, numSamples);
    }

    void Processor::bypassMix(float* const* channels, int numChannels, int numSamples) noexcept
    {
        if (!wet_.moving())                                      // bypassed (processing never gets here)
        {
            for (int c = 0; c < numChannels; ++c)
            {
                if constexpr (product::kIsInstrument)
                    std::fill(channels[c], channels[c] + numSamples, 0.0f);
                else
                    std::copy_n(dry_.data() + static_cast<std::size_t>(c * maxBlock_), numSamples, channels[c]);
            }
            return;
        }
        // The fade: one tick per sample, whatever the channels. Where it has landed (w exactly 0 or 1, also for the
        // rest of the call) the output is exactly the bypassed sound or the Engine's.
        for (int i = 0; i < numSamples; ++i)
            weight_[static_cast<std::size_t>(i)] = wet_.tick();
        for (int c = 0; c < numChannels; ++c)
        {
            float* y = channels[c];
            if constexpr (product::kIsInstrument)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    const float w = weight_[static_cast<std::size_t>(i)];
                    y[i] = w > 0.0f ? y[i] * w : 0.0f;
                }
            }
            else
            {
                const float* x = dry_.data() + static_cast<std::size_t>(c * maxBlock_);
                for (int i = 0; i < numSamples; ++i)
                {
                    const float w = weight_[static_cast<std::size_t>(i)];
                    if (w <= 0.0f)
                        y[i] = x[i];
                    else if (w < 1.0f)
                        y[i] = x[i] * (1.0f - w) + y[i] * w;     // equal-gain: the weights sum to 1
                }
            }
        }
    }

    void Processor::runUnprepared(juce::AudioBuffer<float>& buffer) noexcept
    {
        const int n = buffer.getNumSamples();
        const int numIn = product::kIsInstrument ? 0 : std::min(getTotalNumInputChannels(), buffer.getNumChannels());
        for (int ch = 0; ch < numIn; ++ch)
            fsdsp::sanitize(buffer.getReadPointer(ch), buffer.getWritePointer(ch), n);
        for (int ch = numIn; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, n);
    }

    void Processor::hostBlock(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept
    {
        const juce::ScopedNoDenormals noDenormals;              // the mode the programs are compiled for
        bypassed_.store(bypassed, std::memory_order_relaxed);    // what listen reads (ProcessorLive.cpp)
        hostBlocks_.store(hostBlocks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (buffer.getNumSamples() > largestHostBlock_.load(std::memory_order_relaxed))
            largestHostBlock_.store(buffer.getNumSamples(), std::memory_order_relaxed);
        if (!prepared_)
        {
            runUnprepared(buffer);
            return;
        }
        const float target = bypassed ? 0.0f : 1.0f;
        wet_.setTarget(target);
        if (!wetSet_)                                            // the first block after prepareToPlay: no fade
        {
            wet_.lin.cur = target;
            wetSet_ = true;
        }
        readParameters();
        // The host's transport, for the program's Host block (host.fm): its tempo, position, signature, playing.
        Transport t;
        if (juce::AudioPlayHead* head = getPlayHead())
            if (const auto pos = head->getPosition())
            {
                t.known = true;
                t.bpm = pos->getBpm().orFallback(120.0);
                t.beat = pos->getPpqPosition().orFallback(0.0);
                t.barStart = pos->getPpqPositionOfLastBarStart().orFallback(0.0);
                if (const auto sig = pos->getTimeSignature())
                {
                    t.beatsPerBar = sig->numerator;
                    t.beatUnit = sig->denominator;
                }
                t.playing = pos->getIsPlaying();
            }
        engine_.setTransport(t);
        capture_.begin(buffer.getNumSamples(), bypassed, t, values_, engine_);
        play(buffer, midi, bypassed);
        capture_.end(engine_);
    }

    void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        hostBlock(buffer, midi, false);
    }

    void Processor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        hostBlock(buffer, midi, true);
    }

    // ---- the message thread ------------------------------------------------------------------------------------------
    bool Processor::hasEditor() const { return true; }

    juce::AudioProcessorEditor* Processor::createEditor()
    {
        return playground::createEditor(*this);
    }

    int Processor::getNumPrograms() { return 1; }
    int Processor::getCurrentProgram() { return 0; }
    void Processor::setCurrentProgram(int) {}
    const juce::String Processor::getProgramName(int) { return {}; }
    void Processor::changeProgramName(int, const juce::String&) {}

    Control Processor::control(int c) const
    {
        if (c < 0 || c >= kMaxControls)
            return {};
        const std::lock_guard<std::mutex> lock(programMutex_);
        return controls_[static_cast<std::size_t>(c)];
    }

    std::vector<std::string> Processor::controlOptions(int c) const
    {
        if (c < 0 || c >= kMaxControls)
            return {};
        const std::lock_guard<std::mutex> lock(programMutex_);
        return panel_.options[static_cast<std::size_t>(c)];
    }

    bool Processor::meters(Meters& out) const { return engine_.meters(out); }
    void Processor::scope(bool input, float* out, int n) const { engine_.scope(input, out, n); }

    void Processor::apply(const CompileStatus& s, bool reset)
    {
        if (!s.ok)
            return;                                              // the program that played plays on
        capture_.noteProgram(s.request, s.sources, s.controls);  // a recording's replay compiles these files
        Controls before;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            before = controls_;
            controls_ = s.controls;
            panel_ = s.panel;
            program_ = s.request;
            running_ = s.sources;
        }
        if (reset)
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& now = s.controls[static_cast<std::size_t>(c)];
                const Control& was = before[static_cast<std::size_t>(c)];
                if (!now.used || (was.used && sameName(was, now)))
                    continue;                                    // the same parameter keeps its dial's value
                juce::RangedAudioParameter& p = *params_[static_cast<std::size_t>(c)];
                ownDials_.fetch_or(1u << c);                     // the run's: no move (syncOwnDials)
                p.beginChangeGesture();
                p.setValueNotifyingHost(normOf(now, now.def));
                p.endChangeGesture();
            }
        updateHostDisplay(juce::AudioProcessor::ChangeDetails().withParameterInfoChanged(true));
    }

    // A render's "host" (ChatScenario.h): the Engine's rate, and the host's block as it announced it (the largest it
    // may send; JUCE's own value, set before prepareToPlay).
    double Processor::chatHostRate() const { return static_cast<double>(engine_.rate()); }
    int Processor::chatHostBlock() const
    {
        // The largest block the host has sent since it prepared the plugin (an atomic the audio thread keeps), else what
        // it announced.
        const int seen = largestHostBlock_.load(std::memory_order_relaxed);
        return seen > 0 ? seen : getBlockSize();
    }

    std::string Processor::chatProject() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return projectName_;
    }

    PanelPicture Processor::chatPicture(bool window, const std::string& page)
    {
        std::unique_lock<std::mutex> lock(pictureMutex_);
        pictureAsked_ = true;
        pictureReady_ = false;
        pictureWindow_ = window;
        picturePage_ = page;
        if (!pictureDone_.wait_for(lock, std::chrono::seconds(10), [this] { return pictureReady_; }))
        {
            pictureAsked_ = false;
            PanelPicture none;
            none.why = "the plugin's message thread did not draw the picture in time (the host may be busy); try again";
            return none;
        }
        pictureReady_ = false;
        return std::move(picture_);
    }

    void Processor::poll()
    {
        {
            std::unique_lock<std::mutex> lock(pictureMutex_);
            if (pictureAsked_)
            {
                pictureAsked_ = false;
                const bool window = pictureWindow_;
                const std::string page = picturePage_;
                lock.unlock();
                PanelPicture p = picturePanel(ports(), editorPort(), editorW_.load(), editorH_.load(), window, page);
                lock.lock();
                picture_ = std::move(p);
                pictureReady_ = true;
                pictureDone_.notify_all();
            }
        }
        picturesIdle();                                          // the pictures' GPU context, let go when idle
        journalPoll();                                           // what moved since the last poll (ProcessorLive.cpp)
        if (compiler_.revision() != seen_)
        {
            const CompileStatus s = compiler_.status();
            seen_ = s.revision;
            const uint64_t run = editorRun_.load();
            apply(s, run != 0 && s.request == run);
            if (s.request == userRun_.load())
                journal_.note(Journal::Who::you, runWords(s));
            // A version's program (or a preset's): its dials, by name, once it compiled (an older request's result
            // waits for its own).
            if (dialsRequest_ != 0 && s.request >= dialsRequest_)
            {
                if (s.ok && s.request == dialsRequest_)
                    setDialsByName(s.controls, projectDials_);
                projectDials_.clear();
                dialsRequest_ = 0;
            }
        }
        // Dials Claude set, after the compile that may have brought them: host gestures, on this thread.
        std::vector<std::pair<int, float>> dials;
        {
            const std::lock_guard<std::mutex> lock(dialMutex_);
            dials.swap(pendingDials_);
        }
        for (const auto& [slot, plain] : dials)
        {
            const Control c = control(slot);
            if (!c.used)
                continue;
            juce::RangedAudioParameter& p = *params_[static_cast<std::size_t>(slot)];
            ownDials_.fetch_or(1u << slot);                      // journalled when Claude set it (chatSetDial)
            p.beginChangeGesture();
            p.setValueNotifyingHost(normOf(c, plain));
            p.endChangeGesture();
        }
        capture_.housekeep();                                    // frees what the audio thread handed back
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (const uint32_t a = engine_.takeAlerts(); a != 0)
        {
            alerts_.fetch_or(a);
            alertsAt_ = now;
            journal_.alert(a);
        }
        else if (alerts_.load() != 0 && now - alertsAt_ > kAlertMs)
            alerts_.store(0);
        syncOwnDials();                                          // what poll() itself set: a run's, a version's, Claude's
    }

    void Processor::setEditorSize(int w, int h) noexcept
    {
        const bool ok = w > 0 && h > 0 && w <= kMaxEditorSide && h <= kMaxEditorSide;
        editorW_.store(ok ? w : 0);
        editorH_.store(ok ? h : 0);
    }

    void Processor::settle()
    {
        compiler_.wait();
        poll();
    }

    void Processor::timerCallback()
    {
        poll();
    }

    void Processor::getStateInformation(juce::MemoryBlock& destData)
    {
        juce::ValueTree state = apvts_.copyState();
        juce::ValueTree program{ kProgramTag };
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            program.setProperty(kActive, draft_.active, nullptr);
            if (!projectName_.empty())
            {
                program.setProperty(kProject, juce::String::fromUTF8(projectName_.c_str()), nullptr);
                program.setProperty(kProjectVersion, projectVersion_, nullptr);
                if (!projectPreset_.empty())
                    program.setProperty(kProjectPreset, juce::String::fromUTF8(projectPreset_.c_str()), nullptr);
            }
            program.appendChild(filesTree(kRunTag, running_), nullptr);
            program.appendChild(filesTree(kDraftTag, draft_.files), nullptr);
        }
        state.appendChild(program, nullptr);
        juce::ValueTree chat{ kChatTag };
        chat.setProperty(kJson, juce::String::fromUTF8(chat_.saveState().c_str()), nullptr);
        state.appendChild(chat, nullptr);
        const std::vector<std::string> pages = guiPages();
        if ((editorW_.load() > 0 && editorH_.load() > 0) || panelView_.load() || !chatOpen_.load() || !explorerOpen_.load()
            || !pages.empty())
        {
            juce::ValueTree editor{ kEditorTag };
            if (editorW_.load() > 0 && editorH_.load() > 0)
            {
                editor.setProperty(kWidth, editorW_.load(), nullptr);
                editor.setProperty(kHeight, editorH_.load(), nullptr);
            }
            if (panelView_.load())
                editor.setProperty(kView, "panel", nullptr);
            if (!chatOpen_.load())
                editor.setProperty(kChat, "closed", nullptr);
            if (!explorerOpen_.load())
                editor.setProperty(kExplorer, "closed", nullptr);
            for (const std::string& title : pages)
            {
                juce::ValueTree page{ kPageTag };
                page.setProperty(kTitle, juce::String::fromUTF8(title.c_str()), nullptr);
                editor.appendChild(page, nullptr);
            }
            state.appendChild(editor, nullptr);
        }
        if (const std::unique_ptr<juce::XmlElement> xml = state.createXml())
            copyXmlToBinary(*xml, destData);
    }

    void Processor::setStateInformation(const void* data, int sizeInBytes)
    {
        const std::unique_ptr<juce::XmlElement> xml = getXmlFromBinary(data, sizeInBytes);
        if (xml == nullptr || !xml->hasTagName(apvts_.state.getType()))
            return;
        juce::ValueTree state = juce::ValueTree::fromXml(*xml);
        // A value that is not a finite number (a damaged or crafted state, or one saved after a host wrote a NaN)
        // loads as the middle of the range: a parameter never holds a NaN from a state.
        for (int c = 0; c < kMaxControls; ++c)
        {
            juce::ValueTree p = state.getChildWithProperty(kParamId, juce::var(idOf(c)));
            if (p.isValid() && p.hasProperty(kParamValue)
                && !std::isfinite(static_cast<double>(p.getProperty(kParamValue))))
                p.setProperty(kParamValue, 0.5, nullptr);
        }
        const juce::ValueTree program = state.getChildWithName(kProgramTag);
        Sources run = filesOf(program.getChildWithName(kRunTag), program.getProperty(kSource).toString());
        Draft draft;
        draft.files = filesOf(program.getChildWithName(kDraftTag), program.getProperty(kDraft).toString());
        if (run.empty())
            run = singleFile(kExamples[0].source);
        if (draft.files.empty())
            draft.files = run;
        draft.active = std::clamp(static_cast<int>(program.getProperty(kActive, 0)), 0,
                                  static_cast<int>(draft.files.size()) - 1);
        state.removeChild(program, nullptr);
        const juce::ValueTree chat = state.getChildWithName(kChatTag);
        if (chat.isValid())
        {
            chat_.loadState(chat.getProperty(kJson).toString().toStdString());
            state.removeChild(chat, nullptr);
        }
        const juce::ValueTree editor = state.getChildWithName(kEditorTag);
        panelView_.store(editor.isValid() && editor.getProperty(kView).toString() == "panel");
        chatOpen_.store(!(editor.isValid() && editor.getProperty(kChat).toString() == "closed"));
        explorerOpen_.store(!(editor.isValid() && editor.getProperty(kExplorer).toString() == "closed"));
        {
            std::vector<std::string> pages;
            for (int k = 0; editor.isValid() && k < editor.getNumChildren() && k < 64; ++k)
                if (const juce::ValueTree page = editor.getChild(k); page.hasType(kPageTag))
                    pages.push_back(page.getProperty(kTitle).toString().substring(0, 200).toStdString());
            setGuiPages(pages);
        }
        if (editor.isValid())
        {
            setEditorSize(static_cast<int>(editor.getProperty(kWidth, 0)), static_cast<int>(editor.getProperty(kHeight, 0)));
            state.removeChild(editor, nullptr);
        }
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            // The project open; a session from before projects names a preset, now a project of that name.
            if (program.hasProperty(kProject))
            {
                projectName_ = projectstore::tidyName(program.getProperty(kProject).toString().toStdString());
                projectVersion_ = std::max(0, static_cast<int>(program.getProperty(kProjectVersion, 0)));
                projectPreset_ = projectstore::tidyName(program.getProperty(kProjectPreset).toString().toStdString());
            }
            else
            {
                projectName_ = projectstore::tidyName(program.getProperty(kPreset).toString().toStdString());
                projectVersion_ = 0;
                projectPreset_.clear();
            }
            projectLatest_ = projectVersion_;
            projectFiles_ = draft.files;                         // the session's files are its version's, as saved
            baseDials_.clear();
            draft_ = std::move(draft);
            ++draftRevision_;
        }
        // The session's program, compiled now, its dials as the session has them.
        const CompileStatus s = compiler_.compileNow(std::move(run));
        seen_ = s.revision;
        apply(s, false);
        apvts_.replaceState(state);
        journal_.note(Journal::Who::host, "loaded a saved state (the program, its files and its dials)");
        syncDials();
    }

    // ---- EditorPort ------------------------------------------------------------------------------------------------------
    Draft Processor::draft() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return draft_;
    }

    uint64_t Processor::draftRevision() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return draftRevision_;
    }

    void Processor::setDraft(const Draft& draft)
    {
        Draft before;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            before = std::exchange(draft_, draft);
        }
        journalDraft(before, draft, Journal::Who::you);
    }

    void Processor::run(const Sources& files)
    {
        const uint64_t id = compiler_.request(files);
        editorRun_.store(id);
        userRun_.store(id);
    }

    bool Processor::compiling() const { return compiler_.busy(); }
    CompileStatus Processor::status() const { return compiler_.status(); }
    uint64_t Processor::statusRevision() const { return compiler_.revision(); }
    void Processor::check(const Sources& files) { compiler_.check(files); }
    CheckStatus Processor::checkStatus() const { return compiler_.checkStatus(); }

    // The IR is kept only while someone looks (Compiler::setIrWanted): the first view opened asks for it, the last
    // closed lets it go.
    void Processor::addIrViewer()
    {
        if (irViewers_++ == 0)
            compiler_.setIrWanted(true);
    }

    void Processor::removeIrViewer()
    {
        if (irViewers_ > 0 && --irViewers_ == 0)
            compiler_.setIrWanted(false);
    }

    Sources Processor::running() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return running_;
    }

    Controls Processor::controls() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return controls_;
    }

    PanelSpec Processor::panel() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return panel_;
    }

    uint64_t Processor::program() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return program_;
    }

    float Processor::load() const { return engine_.load(); }
    bool Processor::frozen() const { return engine_.frozen(); }
    uint32_t Processor::alerts() const { return alerts_.load(); }

    // ---- projects --------------------------------------------------------------------------------------------------------
    namespace
    {
        std::string whenOf(int64_t ms) { return juce::Time(static_cast<juce::int64>(ms)).formatted("%Y-%m-%d %H:%M").toStdString(); }

        // `over`'s dials, then `dials` over them, by name.
        std::vector<projectstore::Dial> overlay(std::vector<projectstore::Dial> over, const std::vector<projectstore::Dial>& dials)
        {
            for (const projectstore::Dial& d : dials)
            {
                const auto it = std::find_if(over.begin(), over.end(), [&](const projectstore::Dial& o) { return o.name == d.name; });
                if (it != over.end())
                    it->value = d.value;
                else
                    over.push_back(d);
            }
            return over;
        }
    }

    std::vector<ProjectEntry> Processor::projects() const
    {
        std::vector<ProjectEntry> out;
        const std::vector<projectstore::Entry> list = projectstore::list();
        for (const projectstore::Entry& e : list)
            out.push_back({ e.name, whenOf(e.savedMs), e.latest, e.versions, static_cast<int>(e.presets.size()) });
        const std::lock_guard<std::mutex> lock(programMutex_);
        for (const projectstore::Entry& e : list)
            if (projectstore::sameName(e.name, projectName_))
                projectLatest_ = e.latest;               // another instance may have saved one since
        return out;
    }

    std::string Processor::projectFolder() const
    {
        const juce::String path = projectstore::directory().getFullPathName();
        const juce::String home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
        return (path.startsWith(home + "/") ? "~" + path.substring(home.length()) : path).toStdString();
    }

    ProjectState Processor::project() const
    {
        ProjectState st;
        std::vector<projectstore::Dial> base;
        Controls cs;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            st.name = projectName_;
            st.version = projectVersion_;
            st.latest = std::max(projectLatest_, projectVersion_);
            st.preset = projectPreset_;
            st.edited = !projectName_.empty() && draft_.files != projectFiles_;
            base = baseDials_;
            cs = controls_;
        }
        // The dials against the ones the version or the preset set (not while its compile is on its way).
        if (dialsRequest_ == 0)
            for (int c = 0; c < kMaxControls && !st.dialsEdited; ++c)
            {
                const Control& k = cs[static_cast<std::size_t>(c)];
                if (!k.used)
                    continue;
                const auto it = std::find_if(base.begin(), base.end(), [&](const projectstore::Dial& d) { return d.name == k.name; });
                if (it == base.end())
                    continue;
                const float norm = raw_[static_cast<std::size_t>(c)]->load();
                const float plain = std::isfinite(norm) ? plainOf(k, norm) : k.def;
                st.dialsEdited = std::fabs(plain - it->value) > 1.0e-4f * std::max(1.0e-6f, std::fabs(k.max - k.min));
            }
        return st;
    }

    std::vector<VersionEntry> Processor::versions(const std::string& project) const
    {
        std::vector<VersionEntry> out;
        if (const std::optional<projectstore::Project> p = projectstore::load(project))
            for (auto it = p->versions.rbegin(); it != p->versions.rend(); ++it)
                out.push_back({ it->number, whenOf(it->savedMs), it->note });
        return out;
    }

    std::vector<PresetEntry> Processor::presets(const std::string& project) const
    {
        std::vector<PresetEntry> out;
        if (const std::optional<projectstore::Project> p = projectstore::load(project))
            for (const projectstore::Preset& pr : p->presets)
                out.push_back({ pr.name, whenOf(pr.savedMs), pr.from });
        return out;
    }

    std::vector<projectstore::Dial> Processor::dialsNow() const
    {
        Controls cs;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            cs = controls_;
        }
        std::vector<projectstore::Dial> out;
        for (int c = 0; c < kMaxControls; ++c)
            if (const Control& k = cs[static_cast<std::size_t>(c)]; k.used)
            {
                const float norm = raw_[static_cast<std::size_t>(c)]->load();
                out.push_back({ k.name, std::isfinite(norm) ? plainOf(k, norm) : k.def });
            }
        return out;
    }

    // Host gestures, on the message thread: each dial on the parameter of its name (a name the program lacks: none).
    void Processor::setDialsByName(const Controls& cs, const std::vector<projectstore::Dial>& dials)
    {
        for (const projectstore::Dial& d : dials)
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = cs[static_cast<std::size_t>(c)];
                if (!k.used || d.name != k.name)
                    continue;
                juce::RangedAudioParameter& p = *params_[static_cast<std::size_t>(c)];
                ownDials_.fetch_or(1u << c);                     // a version's or a preset's: its line says so
                p.beginChangeGesture();
                p.setValueNotifyingHost(normOf(k, d.value));
                p.endChangeGesture();
                break;
            }
    }

    bool Processor::openVersion(const projectstore::Project& p, const projectstore::Version& v, std::string& why)
    {
        Draft d;
        d.files = tidyFiles(v.files);
        if (d.files.empty())
        {
            why = "\"" + p.name + "\" version " + std::to_string(v.number) + " has no program";
            return false;
        }
        d.active = std::clamp(v.active, 0, static_cast<int>(d.files.size()) - 1);
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            draft_ = d;
            ++draftRevision_;                                    // the editor shows the version's files
            projectName_ = p.name;
            projectVersion_ = v.number;
            projectLatest_ = p.latest().number;
            projectPreset_.clear();
            projectFiles_ = d.files;
            baseDials_ = v.dials;
        }
        projectDials_ = v.dials;
        const uint64_t id = compiler_.request(d.files);
        dialsRequest_ = id;
        editorRun_.store(id);
        return true;
    }

    bool Processor::openProject(const std::string& name, int version, std::string& why)
    {
        const std::optional<projectstore::Project> p = projectstore::load(name, &why);
        if (!p)
            return false;
        const projectstore::Version* v = version <= 0 ? &p->latest() : p->version(version);
        if (v == nullptr)
        {
            why = "\"" + p->name + "\" keeps no version " + std::to_string(version);
            return false;
        }
        if (!openVersion(*p, *v, why))
            return false;
        journal_.note(Journal::Who::you, "opened project \"" + p->name + "\", version " + std::to_string(v->number));
        return true;
    }

    bool Processor::saveVersion(const std::string& name, SavedVersion& saved, std::string& why)
    {
        Sources files;
        int active = 0;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            files = draft_.files;
            active = draft_.active;
        }
        const std::vector<projectstore::Dial> dials = dialsNow();
        const std::optional<projectstore::Saved> r = projectstore::saveVersion(name, files, active, dials, &why);
        if (!r)
            return false;
        saved = { r->number, r->unchanged, r->created };
        if (!r->unchanged)
            journal_.note(Journal::Who::you, "saved version " + std::to_string(r->number) + " of \"" + projectstore::tidyName(name) + "\"");
        const std::lock_guard<std::mutex> lock(programMutex_);
        const std::string tidy = projectstore::tidyName(name);
        // The preset applied stays named while the dials are still its.
        bool keep = projectstore::sameName(tidy, projectName_) && !projectPreset_.empty();
        for (const projectstore::Dial& d : dials)
            for (const projectstore::Dial& b : baseDials_)
                if (b.name == d.name && b.value != d.value)
                    keep = false;
        if (!keep)
            projectPreset_.clear();
        projectName_ = tidy;
        projectVersion_ = r->number;
        projectLatest_ = r->number;
        projectFiles_ = files;
        baseDials_ = dials;
        return true;
    }

    bool Processor::deleteProject(const std::string& name, std::string& why)
    {
        if (!projectstore::remove(name, &why))
            return false;
        const std::lock_guard<std::mutex> lock(programMutex_);
        if (projectstore::sameName(projectName_, name))
        {
            projectName_.clear();
            projectVersion_ = projectLatest_ = 0;
            projectPreset_.clear();
            baseDials_.clear();
        }
        return true;
    }

    bool Processor::deleteVersion(const std::string& name, int version, std::string& why)
    {
        if (!projectstore::removeVersion(name, version, &why))
            return false;
        const std::lock_guard<std::mutex> lock(programMutex_);
        if (projectstore::sameName(projectName_, name) && projectVersion_ == version)
            projectVersion_ = 0;                                 // the files stay, as a version no more kept
        return true;
    }

    bool Processor::savePreset(const std::string& preset, std::string& why)
    {
        std::string project;
        int version = 0;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            project = projectName_;
            version = projectVersion_;
        }
        if (project.empty())
        {
            why = "save the project first: a preset is kept in its project";
            return false;
        }
        if (!projectstore::exists(project))
        {
            why = "\"" + project + "\" is not on this Mac any more: save it again first";
            return false;
        }
        const std::vector<projectstore::Dial> dials = dialsNow();
        if (!projectstore::savePreset(project, preset, dials, version, &why))
            return false;
        journal_.note(Journal::Who::you, "saved preset \"" + projectstore::tidyName(preset) + "\"");
        const std::lock_guard<std::mutex> lock(programMutex_);
        projectPreset_ = projectstore::tidyName(preset);
        baseDials_ = dials;
        return true;
    }

    bool Processor::applyPreset(const std::string& project, const std::string& preset, std::string& why)
    {
        const std::optional<projectstore::Project> p = projectstore::load(project, &why);
        if (!p)
            return false;
        const projectstore::Preset* pr = p->preset(preset);
        if (pr == nullptr)
        {
            why = "\"" + p->name + "\" has no preset named \"" + preset + "\"";
            return false;
        }
        bool open = false;
        Controls cs;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            open = projectstore::sameName(projectName_, p->name);
            cs = controls_;
        }
        if (!open)
        {
            // Its project's latest version first; the preset's dials over the version's once it compiled.
            if (!openVersion(*p, p->latest(), why))
                return false;
            projectDials_ = overlay(p->latest().dials, pr->dials);
        }
        else if (dialsRequest_ != 0)
            projectDials_ = overlay(projectDials_, pr->dials);   // a compile on its way: after it
        else
            setDialsByName(cs, pr->dials);
        syncOwnDials();                                          // the preset's dials: its line says so
        journal_.note(Journal::Who::you, "applied preset \"" + pr->name + "\" of \"" + p->name + "\"");
        const std::lock_guard<std::mutex> lock(programMutex_);
        projectPreset_ = pr->name;
        baseDials_ = open ? overlay(baseDials_, pr->dials) : overlay(p->latest().dials, pr->dials);
        return true;
    }

    bool Processor::deletePreset(const std::string& project, const std::string& preset, std::string& why)
    {
        if (!projectstore::removePreset(project, preset, &why))
            return false;
        const std::lock_guard<std::mutex> lock(programMutex_);
        if (projectstore::sameName(projectName_, project) && projectstore::sameName(projectPreset_, preset))
            projectPreset_.clear();
        return true;
    }

    ProjectInfo Processor::projectInfo(const std::string& project) const
    {
        ProjectInfo out;
        if (const std::optional<projectstore::Project> p = projectstore::load(project))
            out = { p->info.description, p->info.author, p->info.category, p->info.tags, p->info.genres };
        return out;
    }

    bool Processor::setProjectInfo(const std::string& project, const ProjectInfo& info, std::string& why)
    {
        return projectstore::setInfo(project, { info.description, info.author, info.category, info.tags, info.genres }, &why);
    }

    bool Processor::exportProject(const std::string& project, bool package, bool allVersions, const std::string& path,
                                  Exported& out, std::string& why)
    {
        const std::optional<projectstore::Project> p = projectstore::load(project, &why);
        if (!p)
            return false;
        const juce::File to(juce::String::fromUTF8(path.c_str()));
        if (!juce::File::isAbsolutePath(to.getFullPathName()))
        {
            why = "the file needs a full path";
            return false;
        }
        projectexport::Result r;
        if (!(package ? projectexport::writePackage(*p, allVersions, to, r, &why) : projectexport::writeSource(*p, allVersions, to, r, &why)))
            return false;
        out.bytes = r.bytes;
        out.raw = r.raw;
        out.versions = r.versions;
        out.method = !package ? "zip" : r.method == projectexport::Method::xz ? "xz" : r.method == projectexport::Method::zlib ? "zlib" : "stored";
        return true;
    }

    bool Processor::importProject(const std::string& path, std::string& name, std::string& why)
    {
        const std::optional<projectstore::Project> p = projectexport::read(juce::File(juce::String::fromUTF8(path.c_str())), &why);
        if (!p)
            return false;
        const std::optional<std::string> added = projectstore::add(*p, &why);
        if (!added)
            return false;
        name = *added;
        return true;
    }

    std::string Processor::exportFolder() const
    {
        return juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getFullPathName().toStdString();
    }

    // ---- ChatHost (the agent's thread) -----------------------------------------------------------------------------------
    Draft Processor::chatDraft() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return draft_;
    }

    void Processor::chatSetDraft(const Draft& draft)
    {
        Draft d = draft;
        d.files = tidyFiles(std::move(d.files));
        if (d.files.empty())
            return;
        d.active = std::clamp(d.active, 0, static_cast<int>(d.files.size()) - 1);
        Draft before;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            before = std::exchange(draft_, d);
            ++draftRevision_;                                    // the editor merges it into its tabs
        }
        if (!chat_.mcpWorking())                                 // the plugin's own chat (Claude Desktop knows its own)
            journalDraft(before, d, Journal::Who::chat);
    }

    CompileStatus Processor::chatRun(const Sources& files)
    {
        const uint64_t id = compiler_.request(files);
        editorRun_.store(id);
        compiler_.wait();
        const CompileStatus s = compiler_.status();
        if (!chat_.mcpWorking() && s.request >= id)
            journal_.note(Journal::Who::chat, runWords(s));
        return s.request >= id ? s : CompileStatus{};
    }

    CompileStatus Processor::chatLastStatus() const { return compiler_.status(); }

    Sources Processor::chatRunning() const
    {
        const CompileStatus s = compiler_.status();
        if (s.ok)
            return s.sources;                                    // poll() may not have taken it yet
        const std::lock_guard<std::mutex> lock(programMutex_);
        return running_;
    }

    Controls Processor::chatDials() const
    {
        const CompileStatus s = compiler_.status();
        if (s.ok)
            return s.controls;
        const std::lock_guard<std::mutex> lock(programMutex_);
        return controls_;
    }

    float Processor::chatDialValue(int slot) const
    {
        if (slot < 0 || slot >= kMaxControls)
            return 0.0f;
        const Control c = chatDials()[static_cast<std::size_t>(slot)];
        {
            // A value Claude set that poll() has not applied yet is the dial's now: a measurement right after set_dial
            // renders it (as the parameter will hold it: through its range and steps).
            const std::lock_guard<std::mutex> lock(dialMutex_);
            for (auto it = pendingDials_.rbegin(); it != pendingDials_.rend(); ++it)
                if (it->first == slot)
                    return plainOf(c, normOf(c, it->second));
        }
        const float norm = raw_[static_cast<std::size_t>(slot)]->load();
        return std::isfinite(norm) ? plainOf(c, norm) : c.def;
    }

    void Processor::chatSetDial(int slot, float plain)
    {
        if (slot < 0 || slot >= kMaxControls)
            return;
        if (const Control c = control(slot); c.used && !chat_.mcpWorking())
            journal_.dial(Journal::Who::chat, slot, c.name, dialTextOf(slot, normOf(c, chatDialValue(slot))), dialTextOf(slot, normOf(c, plain)));
        const std::lock_guard<std::mutex> lock(dialMutex_);
        pendingDials_.emplace_back(slot, plain);
    }

    capture::Reply Processor::chatCapture(capture::Action action, const capture::Options& options)
    {
        // restart (next): the files that play compiled again first, so that the recording starts on the program's fresh
        // state (Capture.h); the editor's draft and the dials stay as they are.
        if (action == capture::Action::arm && options.restart && options.mode == capture::Mode::next
            && capture_.audio())
        {
            // A run on its way (the editor's, a version's, a preset's) lands first and is what starts afresh: a request
            // now would replace it, or follow it with the files before it (the agent's worker waits, as chatRun does).
            compiler_.wait();
            const Sources running = chatRunning();
            if (running.empty())
            {
                capture::Reply r;
                r.why = "nothing plays yet to start afresh (run_program plays the editor's files)";
                return r;
            }
            capture::Options o = options;
            o.freshFrom = compiler_.request(running);
            return capture_.act(action, o);
        }
        return capture_.act(action, options);
    }

    float Processor::chatLoad() const { return engine_.load(); }
    uint32_t Processor::chatAlerts() const { return alerts_.load(); }
}

// JUCE's entry point for every format.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new playground::Processor();
}
