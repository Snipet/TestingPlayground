// Source/plugin/Processor.cpp: see Processor.h.
#include "plugin/Processor.h"

#include "plugin/PresetStore.h"

#include "Examples.h"
#include "Product.h"

#include "fsdsp/core/Sanitize.h"

#include <funkgui/gpu/ShaderLibrary.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace testingplayground
{
    namespace
    {
        // The program in the plugin state, beside the parameters: <PROGRAM active=".."> with <RUN> (the files that
        // play) and <DRAFT> (the editor's), each of <FILE name=".." text=".."/>s. The version before files saved
        // <PROGRAM source=".." draft=".."/>, one file each.
        const juce::Identifier kProgramTag{ "PROGRAM" }, kRunTag{ "RUN" }, kDraftTag{ "DRAFT" }, kFileTag{ "FILE" };
        const juce::Identifier kActive{ "active" }, kName{ "name" }, kText{ "text" }, kPreset{ "preset" };
        const juce::Identifier kSource{ "source" }, kDraft{ "draft" };   // the version before files
        const juce::Identifier kChatTag{ "CHAT" }, kJson{ "json" };      // the chat with Claude (ChatAgent::saveState)
        const juce::Identifier kEditorTag{ "EDITOR" }, kWidth{ "w" }, kHeight{ "h" };   // the editor's size
        const juce::Identifier kView{ "view" };                                          // and its view: "panel"
        const juce::Identifier kChat{ "chat" };                                          // its chat: "closed"
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
            portStore_.push_back(std::make_unique<funkgui::JuceParamPort>(*params_[i]));
            ports_.push_back(portStore_.back().get());
        }

        // The first example plays from the start (fminus.examples checks it compiles), its dials at their defaults.
        draft_.files = singleFile(kExamples[0].source);
        presetFiles_ = draft_.files;                             // nothing to lose yet: no preset load asks first
        compiler_.setShaderTools(shaderTools());
        const CompileStatus s = compiler_.compileNow(draft_.files);
        jassert(s.ok);
        seen_ = s.revision;
        apply(s, true);
        startTimerHz(20);
    }

    Processor::~Processor()
    {
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
        maxBlock_ = std::max(1, maximumExpectedSamplesPerBlock);
        largestCall_ = 0;
        engine_.prepare(sampleRate, maxBlock_);
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
            engine_.process(channels.data(), numCh, len, std::span<const Event>(events_.data(), numEvents));
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

    void Processor::playNote(int note, float velocity)
    {
        if constexpr (product::kIsInstrument)
        {
            if (note < 0 || note > 127 || !std::isfinite(velocity))
                return;
            const uint32_t head = uiNoteHead_.load(std::memory_order_relaxed);
            if (head - uiNoteTail_.load(std::memory_order_acquire) >= kUiNotes)
                return;                                          // full: dropped
            Event e;
            e.note = static_cast<uint8_t>(note);
            e.velocity = std::clamp(velocity, 0.0f, 1.0f);
            e.kind = e.velocity > 0.0f ? EventKind::noteOn : EventKind::noteOff;
            uiNotes_[head % kUiNotes] = e;
            uiNoteHead_.store(head + 1, std::memory_order_release);
        }
        else
            juce::ignoreUnused(note, velocity);
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
        play(buffer, midi, bypassed);
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
        return testingplayground::createEditor(*this);
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
                p.beginChangeGesture();
                p.setValueNotifyingHost(normOf(now, now.def));
                p.endChangeGesture();
            }
        updateHostDisplay(juce::AudioProcessor::ChangeDetails().withParameterInfoChanged(true));
    }

    void Processor::poll()
    {
        if (compiler_.revision() != seen_)
        {
            const CompileStatus s = compiler_.status();
            seen_ = s.revision;
            const uint64_t run = editorRun_.load();
            apply(s, run != 0 && s.request == run);
            // A preset's program: its dials, by name, once it compiled (an older request's result waits for its own).
            if (presetRequest_ != 0 && s.request >= presetRequest_)
            {
                if (s.ok && s.request == presetRequest_)
                    for (const auto& [name, plain] : presetDials_)
                        for (int c = 0; c < kMaxControls; ++c)
                        {
                            const Control& k = s.controls[static_cast<std::size_t>(c)];
                            if (!k.used || name != k.name)
                                continue;
                            juce::RangedAudioParameter& p = *params_[static_cast<std::size_t>(c)];
                            p.beginChangeGesture();
                            p.setValueNotifyingHost(normOf(k, plain));
                            p.endChangeGesture();
                            break;
                        }
                presetDials_.clear();
                presetRequest_ = 0;
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
            p.beginChangeGesture();
            p.setValueNotifyingHost(normOf(c, plain));
            p.endChangeGesture();
        }
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (const uint32_t a = engine_.takeAlerts(); a != 0)
        {
            alerts_.fetch_or(a);
            alertsAt_ = now;
        }
        else if (alerts_.load() != 0 && now - alertsAt_ > kAlertMs)
            alerts_.store(0);
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
            if (!presetName_.empty())
                program.setProperty(kPreset, juce::String::fromUTF8(presetName_.c_str()), nullptr);
            program.appendChild(filesTree(kRunTag, running_), nullptr);
            program.appendChild(filesTree(kDraftTag, draft_.files), nullptr);
        }
        state.appendChild(program, nullptr);
        juce::ValueTree chat{ kChatTag };
        chat.setProperty(kJson, juce::String::fromUTF8(chat_.saveState().c_str()), nullptr);
        state.appendChild(chat, nullptr);
        const std::vector<std::string> pages = guiPages();
        if ((editorW_.load() > 0 && editorH_.load() > 0) || panelView_.load() || !chatOpen_.load() || !pages.empty())
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
            presetName_ = presetstore::tidyName(program.getProperty(kPreset).toString().toStdString());
            presetFiles_ = draft.files;                          // the session's files are its preset's, as saved
            draft_ = std::move(draft);
            ++draftRevision_;
        }
        // The session's program, compiled now, its dials as the session has them.
        const CompileStatus s = compiler_.compileNow(std::move(run));
        seen_ = s.revision;
        apply(s, false);
        apvts_.replaceState(state);
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
        const std::lock_guard<std::mutex> lock(programMutex_);
        draft_ = draft;
    }

    void Processor::run(const Sources& files)
    {
        editorRun_.store(compiler_.request(files));
    }

    bool Processor::compiling() const { return compiler_.busy(); }
    CompileStatus Processor::status() const { return compiler_.status(); }
    uint64_t Processor::statusRevision() const { return compiler_.revision(); }

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
    uint32_t Processor::alerts() const { return alerts_.load(); }

    // ---- presets ---------------------------------------------------------------------------------------------------------
    std::vector<PresetEntry> Processor::presets() const
    {
        std::vector<PresetEntry> out;
        for (const presetstore::Entry& e : presetstore::list())
            out.push_back({ e.name, juce::Time(e.modifiedMs).formatted("%Y-%m-%d %H:%M").toStdString() });
        return out;
    }

    std::string Processor::presetFolder() const
    {
        const juce::String path = presetstore::directory().getFullPathName();
        const juce::String home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
        return (path.startsWith(home + "/") ? "~" + path.substring(home.length()) : path).toStdString();
    }

    bool Processor::savePreset(const std::string& name, std::string& why)
    {
        presetstore::Preset p;
        p.name = presetstore::tidyName(name);
        Controls cs;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            p.files = draft_.files;
            p.active = draft_.active;
            cs = controls_;
        }
        for (int c = 0; c < kMaxControls; ++c)
            if (const Control& k = cs[static_cast<std::size_t>(c)]; k.used)
            {
                const float norm = raw_[static_cast<std::size_t>(c)]->load();
                p.dials.push_back({ k.name, std::isfinite(norm) ? plainOf(k, norm) : k.def });
            }
        if (!presetstore::save(p, &why))
            return false;
        const std::lock_guard<std::mutex> lock(programMutex_);
        presetName_ = p.name;
        presetFiles_ = p.files;
        return true;
    }

    bool Processor::loadPreset(const std::string& name, std::string& why)
    {
        std::optional<presetstore::Preset> p = presetstore::load(name, &why);
        if (!p)
            return false;
        Draft d;
        d.files = tidyFiles(std::move(p->files));
        if (d.files.empty())
        {
            why = "\"" + p->name + "\" has no program";
            return false;
        }
        d.active = std::clamp(p->active, 0, static_cast<int>(d.files.size()) - 1);
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            draft_ = d;
            ++draftRevision_;                                    // the editor shows the preset's files
            presetName_ = p->name;
            presetFiles_ = d.files;
        }
        presetDials_.clear();
        for (const presetstore::Dial& dial : p->dials)
            presetDials_.emplace_back(dial.name, dial.value);
        const uint64_t id = compiler_.request(d.files);
        presetRequest_ = id;
        editorRun_.store(id);
        return true;
    }

    bool Processor::deletePreset(const std::string& name, std::string& why)
    {
        if (!presetstore::remove(name, &why))
            return false;
        const std::lock_guard<std::mutex> lock(programMutex_);
        if (juce::String(presetName_).equalsIgnoreCase(juce::String(name)))
            presetName_.clear();
        return true;
    }

    std::string Processor::presetName() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return presetName_;
    }

    bool Processor::presetEdited() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return draft_.files != presetFiles_;
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
        const std::lock_guard<std::mutex> lock(programMutex_);
        draft_ = std::move(d);
        ++draftRevision_;                                        // the editor merges it into its tabs
    }

    CompileStatus Processor::chatRun(const Sources& files)
    {
        const uint64_t id = compiler_.request(files);
        editorRun_.store(id);
        compiler_.wait();
        const CompileStatus s = compiler_.status();
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
        const float norm = raw_[static_cast<std::size_t>(slot)]->load();
        return std::isfinite(norm) ? plainOf(c, norm) : c.def;
    }

    void Processor::chatSetDial(int slot, float plain)
    {
        if (slot < 0 || slot >= kMaxControls)
            return;
        const std::lock_guard<std::mutex> lock(dialMutex_);
        pendingDials_.emplace_back(slot, plain);
    }

    float Processor::chatLoad() const { return engine_.load(); }
    uint32_t Processor::chatAlerts() const { return alerts_.load(); }
}

// JUCE's entry point for every format.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new testingplayground::Processor();
}
