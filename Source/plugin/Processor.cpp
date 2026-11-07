// Source/plugin/Processor.cpp: see Processor.h.
#include "plugin/Processor.h"

#include "plugin/ChatStore.h"
#include "plugin/Factory.h"
#include "plugin/ProjectExport.h"
#include "plugin/ProjectStore.h"
#include "plugin/WheelMap.h"

#include "Examples.h"
#include "Product.h"

#include "fminus/CirDoc.h"
#include "fminus/Circuits.h"
#include "fsdsp/core/Sanitize.h"

#include <funkgui/gpu/ShaderLibrary.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace playground
{
    namespace
    {
        // The program in the plugin state, beside the parameters: <PROGRAM active=".."> with <RUN> (the files that
        // play) and <DRAFT> (the editor's), each of <FILE name=".." text=".."/>s, and <CIRCUITS> when its netlists
        // have the circuit self-test's answers (fminus/Circuits.h), each an <ANSWER key=".." text=".."/>: stored
        // before the program compiles on load, so it plays its refined configuration from its first block (a build
        // from before circuits reads past them). The version before files saved <PROGRAM source=".." draft=".."/>,
        // one file each.
        const juce::Identifier kProgramTag{ "PROGRAM" }, kRunTag{ "RUN" }, kDraftTag{ "DRAFT" }, kFileTag{ "FILE" };
        const juce::Identifier kCircuitsTag{ "CIRCUITS" }, kAnswerTag{ "ANSWER" }, kKey{ "key" };
        const juce::Identifier kActive{ "active" }, kName{ "name" }, kText{ "text" }, kPreset{ "preset" },
                               kProject{ "project" }, kProjectVersion{ "projectVersion" }, kProjectPreset{ "projectPreset" },
                               kProjectId{ "projectId" };
        const juce::Identifier kSource{ "source" }, kDraft{ "draft" };   // the version before files
        // The chat with Claude: which one it is, never what was said (ChatAgent.h, "The chats"; CLAUDE.md, "The chats: on
        // this Mac only"): <CHAT instance=".." owner=".." chat=".."/>, the instance's id (its chats while no project is
        // open), the chat's owner and id. A session from before kept the conversation itself (json=".."): it is kept on
        // this Mac when the session loads (ChatAgent::adoptChat), and the session's next save leaves it out.
        const juce::Identifier kChatTag{ "CHAT" }, kJson{ "json" }, kInstance{ "instance" }, kOwner{ "owner" }, kChatId{ "chat" };
        // MCP's marks (ChatAgent::mcpMarksToKeep, Live.h's KeptMark): <MCP><MARK token><FILE name hash netlist lines/>
        // </MARK></MCP>, the hashes in hex, so that get_status `since` with a token from before the plugin was opened
        // again still says which files changed (a state from before them has none).
        const juce::Identifier kMcpTag{ "MCP" }, kMarkTag{ "MARK" }, kToken{ "token" }, kHash{ "hash" }, kNetlist{ "netlist" },
                               kLines{ "lines" };
        // <MCP desktopName="Bass">: the user's name for the instance (McpLink::setName), when it has one; a state without
        // one leaves the name as it is.
        const juce::Identifier kDesktopName{ "desktopName" };
        const juce::Identifier kEditorTag{ "EDITOR" }, kWidth{ "w" }, kHeight{ "h" };   // the editor's size
        const juce::Identifier kView{ "view" };                                          // and its view: "panel"
        // its CODE view's panel under the code and under the interface's files: "hidden"
        const juce::Identifier kCodePanel{ "codePanel" }, kGuiPanel{ "guiPanel" };
        const juce::Identifier kChat{ "chat" };                                          // its chat: "closed"
        const juce::Identifier kExplorer{ "explorer" };                                  // its explorer: "closed"
        const juce::Identifier kExplorerW{ "explorerW" }, kChatW{ "chatW" };             // the widths its user set
        // its code window: "out" while the editor is in it; its size (logical px) and its place (screen points)
        const juce::Identifier kCodeWindow{ "codeWindow" }, kCodeW{ "codeW" }, kCodeH{ "codeH" }, kCodeX{ "codeX" },
                               kCodeY{ "codeY" };
        const juce::Identifier kPageTag{ "PAGE" }, kTitle{ "title" };                    // its interface's pages
        // its netlists' views: <FILE name view="schematic" zoom x y><PROBE node/></FILE> (EditorPort::fileViews)
        const juce::Identifier kFileViewTag{ "FILE" }, kZoom{ "zoom" }, kX{ "x" }, kY{ "y" }, kProbeTag{ "PROBE" }, kNode{ "node" };
        // The folder while it is on (FolderSync.h): <FOLDER name=".." run="1"/> (a state from before it has none)
        const juce::Identifier kFolderTag{ "FOLDER" }, kRunOnSave{ "run" };
        // The panel's own dials (Controls.h's isPanelDial), all 32 as the host's <PARAM>s are:
        // <PANEL><DIAL slot="32" value="0.5"/>...</PANEL>, normalised in the program's controls as those are (a state
        // from before them has none: their defaults).
        const juce::Identifier kPanelTag{ "PANEL" }, kDialTag{ "DIAL" }, kSlot{ "slot" };
        // MIDI learn's mappings (MidiLearn.h): <MIDI><MAP dial="cutoff" channel="1" cc="74" fine="1"/>...</MIDI> (a
        // state without it has none)
        const juce::Identifier kMidiTag{ "MIDI" }, kMapTag{ "MAP" }, kDial{ "dial" }, kChannel{ "channel" }, kCc{ "cc" },
            kFine{ "fine" };
        constexpr int kMaxEditorSide = 16384;

        // The instance ids this process's processors hold (their chats while no project is open: ChatAgent.h, "The
        // chats"): a state another live instance's id is in (a track duplicated, a default preset applied to a new
        // instance) gives this one an id of its own.
        std::mutex heldMutex;
        std::multiset<std::string> heldIds;

        void holdId(const std::string& id)
        {
            const std::lock_guard<std::mutex> lock(heldMutex);
            heldIds.insert(id);
        }

        void releaseId(const std::string& id)
        {
            const std::lock_guard<std::mutex> lock(heldMutex);
            if (const auto it = heldIds.find(id); it != heldIds.end())
                heldIds.erase(it);
        }

        // `mine` given up for `next` (one lock: two processors loading one state at once each find the other's);
        // false, and `mine` kept, when another processor holds `next`.
        bool takeId(const std::string& mine, const std::string& next)
        {
            const std::lock_guard<std::mutex> lock(heldMutex);
            if (next == mine)
                return true;
            if (heldIds.count(next) != 0)
                return false;
            if (const auto it = heldIds.find(mine); it != heldIds.end())
                heldIds.erase(it);
            heldIds.insert(next);
            return true;
        }

        // What building a .gui file's shaders needs (fminus/Shaders.h): shaderc, bundled in the plugin
        // (Contents/Resources/shaderc: CMakeLists.txt copies the pinned bgfx's there; on Windows shaderc.exe, there in
        // the VST3 and beside the Standalone's .exe; on Linux there in the VST3 and beside the Standalone's
        // executable), else where <prefix>SHADERC says (the probes'); FunkGui's
        // prelude, varyings and bgfx_shader.sh; the renderer's target and check; the disk cache's folder.
        ShaderTools shaderTools()
        {
            ShaderTools t;
            // The binary this code is in: the plug-in's (the VST3's Contents/x86_64-win/<Name>.vst3, its
            // Contents/x86_64-linux/<Name>.so, the AU's), or the Standalone's.
            const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
#if defined(_WIN32)
            juce::File bundled = exe.getParentDirectory().getSiblingFile("Resources").getChildFile("shaderc.exe");
            if (!bundled.existsAsFile())
                bundled = exe.getSiblingFile("shaderc.exe");
#elif !defined(__APPLE__)
            juce::File bundled = exe.getParentDirectory().getSiblingFile("Resources").getChildFile("shaderc");
            if (!bundled.existsAsFile())
                bundled = exe.getSiblingFile("shaderc");
#else
            const juce::File bundled = exe.getParentDirectory().getSiblingFile("Resources").getChildFile("shaderc");
#endif
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
#elif defined(_WIN32)
            t.platform = "windows";
            t.profile = "s_5_0";
#else
            t.platform = "linux";
            t.profile = "spirv";
#endif
            // The renderer's own check of a built shader (on macOS Metal's compiler), on the compiler's thread: the
            // message thread's shaders::set() then finds the code checked and compiles nothing (FunkGui v0.25.0).
            t.check = [](const std::vector<uint8_t>& binary, std::string& why) {
                return funkgui::shaders::check(binary.data(), static_cast<uint32_t>(binary.size()), why);
            };
            t.cacheDir = shaderCacheFolder();          // the builds kept between sessions (the user's caches)
            return t;
        }
        const juce::Identifier kParamId{ "id" }, kParamValue{ "value" };   // the APVTS's <PARAM id value> children

        constexpr int kMaxChannels = 8;                          // handed to the Engine (the layouts allow two)
        constexpr double kAlertMs = 2000.0;                      // how long an alert stays shown

        juce::String idOf(int c) { return "p" + juce::String(c + 1); }

        // A panel dial's port (Controls.h's isPanelDial): its value the processor's atomic, written through `write` (the
        // processor's writeDial, which tells the host its state changed), no host parameter behind it (native: none).
        class PanelDialPort final : public funkgui::ParamPort
        {
        public:
            PanelDialPort(int slot, const std::atomic<float>& value, std::function<void(int, float)> write)
                : slot_(slot), value_(value), write_(std::move(write)), id_("d" + std::to_string(slot + 1)) {}

            float value01() const override { return value_.load(std::memory_order_relaxed); }
            float default01() const override { return 0.5f; }               // as a host parameter's (ControlParameter)
            int numSteps() const override { return juce::AudioProcessor::getDefaultNumParameterSteps(); }
            void beginGesture() override {}
            void setValue01(float v) override { write_(slot_, v); }
            void endGesture() override {}
            const char* id() const override { return id_.c_str(); }
            void* native() const override { return nullptr; }

        private:
            int slot_;
            const std::atomic<float>& value_;
            std::function<void(int, float)> write_;
            std::string id_;
        };

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
        // the controllers All Sound Off (120) and All Notes Off (123), both allNotesOff, the mod wheel, the sustain
        // pedal and a racing wheel's (wheelcc: 16 to 24, 48 to 51). The caller sets the offset. (An effect never
        // calls it: maybe_unused.)
        constexpr bool isWheelCc(uint8_t cc) noexcept
        {
            using namespace wheelcc;
            return (cc >= kSteering && cc < kButtons + kButtonCcs) || (cc >= kSteering + kLsb && cc < kSteering + kLsb + kAxes);
        }

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
            else if (status == 0xb0 && (d1 == 1 || d1 == 64 || isWheelCc(d1)))   // the mod wheel, the sustain pedal
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
        for (int c = 0; c < kHostControls; ++c)                  // the panel's own dials are none (Controls.h)
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
            if (isPanelDial(c))
            {
                std::atomic<float>& v = panelValues_[static_cast<std::size_t>(c - kHostControls)];
                v.store(dialDefault01(c));
                raw_[i] = &v;
                portStore_.push_back(std::make_unique<GesturePort>(std::make_unique<PanelDialPort>(
                    c, v, [this](int slot, float norm) { (void)writeDial(slot, norm); })));
            }
            else
            {
                raw_[i] = apvts_.getRawParameterValue(idOf(c));
                params_[i] = apvts_.getParameter(idOf(c));
                jassert(raw_[i] != nullptr && params_[i] != nullptr);
                portStore_.push_back(std::make_unique<GesturePort>(*params_[i]));
            }
            ports_.push_back(portStore_.back().get());
        }

        // The first example, Init, plays from the start (fminus.examples and fminus.init check it), its dials at their
        // defaults.
        draft_.files = singleFile(kExamples[0].source);
        projectFiles_ = draft_.files;                            // nothing to lose yet: no project's open asks first
        compiler_.setShaderTools(shaderTools());
        compiler_.setDialCarrier([this](const Controls& controls, const PanelSpec& panel) { return carryFor(controls, panel); });
        capture_.setOwnCode([this](bool on) { engine_.setOwnCode(on); });
        const CompileStatus s = compiler_.compileNow(draft_.files);
        jassert(s.ok);
        seen_ = s.revision;
        apply(s, true, true, Given{});
        syncDials();                                             // the journal's dials start here
        startTimerHz(20);
        // The chat's owner: this instance (no project is open yet).
        holdId(instanceId_);
        chat_.setOwner(chatstore::instanceOwner(instanceId_), {}, ChatAgent::OwnerChange::carry);
        mcp_ = std::make_unique<McpLink>(McpLink::Hooks{ [this] { return mcpInfo(); },
                                                         [this] {
                                                             std::pair<std::string, std::string> place;
                                                             {
                                                                 const std::lock_guard<std::mutex> lock(trackMutex_);
                                                                 place.first = trackName_;
                                                             }
                                                             const std::lock_guard<std::mutex> lock(programMutex_);
                                                             place.second = projectName_;
                                                             return place;
                                                         },
                                                         [this](const std::string& name, const juce::var& arguments, uint64_t call) {
                                                             return chat_.mcpCall(name, arguments, call);
                                                         },
                                                         [this](uint64_t call) { chat_.mcpCancel(call); } });
    }

    // Claude Desktop and this instance, as the explorer and the chat's header show it (any thread).
    DesktopState Processor::desktop() const
    {
        DesktopState d;
        if (mcp_ == nullptr || !mcp_->listening())
            return d;
        const McpLink::View v = mcp_->view();
        d.linked = true;
        d.id = mcp_->id();
        d.name = mcp_->name();
        d.number = mcp_->numberName();
        d.connected = mcp_->connected();
        d.chosen = v.chosen == d.id;
        d.target = d.chosen || (v.chosen.empty() && v.others.empty());   // chosen, else the only one open
        for (const McpLink::Peer& p : v.others)
        {
            d.others.push_back({ p.id, p.name, p.track, p.project, p.id == v.chosen });
            if (p.id == v.chosen)
                d.targetName = p.name;
        }
        d.doing = v.tool.empty() ? std::string() : ChatAgent::mcpTitle(v.tool);
        d.running = v.running;
        d.ago = v.ago;
        return d;
    }

    bool Processor::setDesktopName(const std::string& name, std::string& why)
    {
        if (mcp_ == nullptr || !mcp_->listening())
        {
            why = "Claude Desktop cannot reach this instance";
            return false;
        }
        const std::string before = mcp_->userName();
        if (!mcp_->setName(name, why))
            return false;
        // No parameter moved, but the session did (it keeps the name): a host that saves on change (VST3's dirty flag)
        // is told. The user's rename alone: a session's restore names it straight (setStateInformation).
        if (mcp_->userName() != before)
            updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
        return true;
    }

    void Processor::chooseDesktop(const std::string& id, bool on)
    {
        if (mcp_ != nullptr)
            mcp_->choose(id, on);
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
        const Assets samples = assetsNow();
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            o->setProperty("project", juce::String::fromUTF8(projectName_.c_str()));
            o->setProperty("version", projectVersion_);
            o->setProperty("preset", juce::String::fromUTF8(projectPreset_.c_str()));
            o->setProperty("edited", !projectName_.empty()
                                         && (draft_.files != projectFiles_ || !assets::same(samples.get(), projectAssets_.get())));
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
        midiTimer_.stopTimer();
        endMidiGestures();                                       // a touch of MIDI's still open, closed for the host
        const std::lock_guard<std::mutex> lock(programMutex_);
        releaseId(instanceId_);
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
        trimFade_.prepare(engine_.rate());
        trimSet_ = false;
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
        // Whose controls the values are in first (apply() stores it after its writes), then the values (Engine.h, "The
        // dials' carry"). A host can write a NaN (JUCE's ranges pass it through): such a value keeps the last good one.
        const uint64_t meaning = meaningProgram_.load(std::memory_order_acquire);
        for (std::size_t c = 0; c < raw_.size(); ++c)
            if (const float v = raw_[c]->load(std::memory_order_relaxed); std::isfinite(v))
                values_[c] = std::clamp(v, 0.0f, 1.0f);
        engine_.setControls(values_, meaning);
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
            trimGate();                                          // the A/B's: a lower program waits for the trim
            engine_.process(channels.data(), numCh, len, std::span<const Event>(events_.data(), numEvents));
            capture_.output(channels.data(), numCh, len);
            trimOutput(channels.data(), numCh, len);             // the A/B's matched loudness (ProcessorAb.cpp)
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
                if (m.numBytes >= 2 && m.data[0] == 0xc0)        // a program change on channel 1: poll() applies it
                {
                    const uint32_t before = midiProgram_.load(std::memory_order_relaxed);
                    midiProgram_.store((((before >> 8) + 1u) << 8) | (m.data[1] & 0x7fu), std::memory_order_release);
                    continue;
                }
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

    CodeWindow Processor::codeWindow() const
    {
        const std::lock_guard<std::mutex> lock(pagesMutex_);
        return codeWindow_;
    }

    void Processor::setCodeWindow(const CodeWindow& window)
    {
        // A size within the editor's sides (else none: the default), a place within a screen's reach.
        CodeWindow w = window;
        if (w.width <= 0 || w.height <= 0 || w.width > kMaxEditorSide || w.height > kMaxEditorSide)
            w.width = w.height = 0;
        if (!w.placed || std::abs(w.x) > 4 * kMaxEditorSide || std::abs(w.y) > 4 * kMaxEditorSide)
        {
            w.placed = false;
            w.x = w.y = 0;
        }
        const std::lock_guard<std::mutex> lock(pagesMutex_);
        codeWindow_ = w;
    }

    // ---- the folder (FolderSync.h; message thread) ------------------------------------------------------------------
    double Processor::seconds() { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }

    // Under the projects' folder (a probe's sandbox under CTest), where a sandboxed host lets the plugin write.
    std::filesystem::path Processor::folderRoot() const
    {
        const juce::File root = projectstore::directory().getChildFile("Folders");
#if defined(_WIN32)
        return std::filesystem::path(root.getFullPathName().toWideCharPointer());
#else
        return std::filesystem::path(root.getFullPathName().toStdString());
#endif
    }

    void Processor::keepFolder()
    {
        folderKeptAt_ = folder_.revision();
        FolderKept k;
        k.on = folder_.on();
        k.name = folder_.name();
        k.run = folder_.runOnSave();
        const std::lock_guard<std::mutex> lock(folderMutex_);
        folderKept_ = std::move(k);
    }

    FolderState Processor::folder() const
    {
        std::string shown;
        if (folder_.on())
        {
            const std::u8string u = folder_.folder().u8string();
            const juce::String path = juce::String::fromUTF8(reinterpret_cast<const char*>(u.data()), static_cast<int>(u.size()));
            const juce::String home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
            shown = (path.startsWith(home + juce::File::getSeparatorString()) ? "~" + path.substring(home.length()) : path)
                        .toStdString();
        }
        return folder_.state(shown);
    }

    bool Processor::setFolderSync(bool on, std::string& why)
    {
        if (!on)
        {
            folder_.stop();
            keepFolder();
            return true;
        }
        if (folder_.on())
            return true;
        // The project open's name, else the instance's ("Synth 2"), else the plugin's.
        std::string name;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            name = ownName(projectName_);                        // a factory project's: its own name
        }
        if (name.empty())
            name = desktopName();
        if (name.empty())
            name = product::kName;
        const bool ok = folder_.start(folderRoot(), name, seconds(), why);
        keepFolder();
        return ok;
    }

    void Processor::setFolderRunOnSave(bool on)
    {
        folder_.setRunOnSave(on);
        keepFolder();
    }

    bool Processor::resolveFolderConflict(const std::string& file, bool keepPlugin, std::string& why)
    {
        return folder_.resolve(file, keepPlugin, seconds(), why);
    }

    bool Processor::openFolder(FolderApp app, std::string& why)
    {
        if (!folder_.on())
        {
            why = "the folder is off";
            return false;
        }
        const std::u8string u = folder_.folder().u8string();
        const std::string path(u.begin(), u.end());
        if (folderLauncher_)
            return folderLauncher_(app, path, why);
        // Never from a probe (CTest's sandbox): it launches nothing on the machine that runs it.
        if (const char* sandbox = std::getenv((std::string(product::kEnvPrefix) + "PREFS_DIR").c_str());
            sandbox != nullptr && *sandbox != '\0')
        {
            why = "nothing opens under a probe";
            return false;
        }
        if (app == FolderApp::files)
        {
            if (juce::File(juce::String::fromUTF8(path.c_str())).startAsProcess())
                return true;
            why = "the folder did not open";
            return false;
        }
        // VS Code by its URL scheme, vscode://file/<path>/ (a folder), which the system hands to VS Code when it is
        // installed: no process of ours, so it works in a sandboxed host too.
        std::string url = "vscode://file";
        if (!path.empty() && path.front() != '/' && path.front() != '\\')
            url += '/';                                  // Windows' "C:\..."
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (const char c : path)
        {
            const auto b = static_cast<unsigned char>(c);
            if (std::isalnum(b) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':')
                url += c;
            else if (c == '\\')
                url += '/';
            else
            {
                url += '%';
                url += kHex[b >> 4];
                url += kHex[b & 15];
            }
        }
        url += '/';
        if (juce::Process::openDocument(juce::String::fromUTF8(url.c_str()), {}))
            return true;
        why = "VS Code did not open (is it installed?)";
        return false;
    }

    // A save in the folder, into the draft (FolderSync's hook): only while the draft is what the pass read, so a write
    // of Claude's or the editor's typing that came since is never overwritten; the editor merges it as one undo step.
    bool Processor::folderDraftIf(const Draft& now, const Draft& next)
    {
        Draft before;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            if (draft_ != now)
                return false;
            before = std::exchange(draft_, next);
            ++draftRevision_;                                    // the editor merges it into its tabs
        }
        journalDraft(before, next, Journal::Who::user);
        return true;
    }

    std::vector<FileView> Processor::fileViews() const
    {
        const std::lock_guard<std::mutex> lock(pagesMutex_);
        return fileViews_;
    }

    void Processor::setFileViews(const std::vector<FileView>& views)
    {
        std::vector<FileView> kept;
        for (const FileView& v : views)
        {
            if (kept.size() >= kMaxFileViews || !isCircuitFile(v.file) || !std::isfinite(v.zoom) || !std::isfinite(v.x)
                || !std::isfinite(v.y))
                continue;
            FileView k = v;
            if (k.probes.size() > kMaxFileProbes)
                k.probes.resize(kMaxFileProbes);
            kept.push_back(std::move(k));
        }
        const std::lock_guard<std::mutex> lock(pagesMutex_);
        fileViews_ = std::move(kept);
    }

    void Processor::setSchematicPins(const std::string& file, const std::string& lines)
    {
        const std::lock_guard<std::mutex> lock(pinsMutex_);
        if (lines.empty())
            schematicPins_.erase(file);
        else if (schematicPins_.size() < 4 * kMaxFiles || schematicPins_.count(file) != 0)
            schematicPins_[file] = lines;               // a file renamed or gone leaves a few entries behind: bounded
    }

    std::string Processor::schematicPins(const std::string& file) const
    {
        const std::lock_guard<std::mutex> lock(pinsMutex_);
        const auto it = schematicPins_.find(file);
        return it == schematicPins_.end() ? std::string() : it->second;
    }

    void Processor::setWindowHooks(WindowHooks hooks)
    {
        windowHooks_ = std::move(hooks);
        wheelKnown_ = false;                                     // the racing wheel read again from these (syncWheel)
        syncWheel();
    }

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
        midiControls(midi);                                      // MIDI learn's (bypassed or not, as automation)
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

    // The block's control changes to MIDI learn (MidiLearn.h), while it listens: a dial learning, or a mapping bound.
    void Processor::midiControls(const juce::MidiBuffer& midi) noexcept
    {
        if (!learn_.listening())
            return;
        for (const juce::MidiMessageMetadata m : midi)
            if (m.numBytes >= 3 && (m.data[0] & 0xf0) == 0xb0)
                learn_.controlChange(m.data[0] & 0x0f, m.data[1] & 0x7f, m.data[2] & 0x7f);
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

    // The host's programs: the project open's presets (refreshPresets keeps them; never a file read here).
    int Processor::getNumPrograms() { return programCount(); }
    int Processor::getCurrentProgram() { return std::clamp(hostCurrent_.load(), 0, programCount() - 1); }

    void Processor::setCurrentProgram(int index)
    {
        // A host may ask on another thread: the message thread applies it (poll), as it applies every preset.
        if (juce::MessageManager::existsAndIsCurrentThread())
            (void)choosePreset(index, "the host");
        else
            hostProgramAsked_.store(index);
    }

    const juce::String Processor::getProgramName(int index)
    {
        const std::lock_guard<std::mutex> lock(presetsMutex_);
        if (index >= 0 && index < static_cast<int>(heldPresets_.size()))
            return juce::String::fromUTF8(heldPresets_[static_cast<std::size_t>(index)].name.c_str());
        if (heldPresets_.empty() && index == 0)
            return heldProject_.empty() ? "(no project open)" : "(no presets)";
        return "-";
    }

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

    void Processor::apply(const CompileStatus& s, bool run, bool loaded, const Given& given)
    {
        Controls controls;
        std::array<std::vector<std::string>, kMaxControls> options;
        if (!s.ok && !s.restored.empty())
        {
            // A program that stopped as it played, and the one before it played again (Compiler.h, "A program that
            // stops"): its files, dials and panel, and the stopped one's files what played before them.
            capture_.noteProgram(s.program, s.restored, s.controls, s.circuits);
            {
                const std::lock_guard<std::mutex> lock(programMutex_);
                if (s.program == program_)
                    return;
                controls_ = s.controls;
                panel_ = s.panel;
                program_ = s.program;
                previous_ = running_;
                previousAssets_ = runningAssets_;
                running_ = s.restored;
                runningAssets_ = s.assets;
                runningSamples_ = s.samples;
            }
            learn_.bind(s.controls);                             // MIDI learn's mappings follow the dials by name
            if (run)
                writeCarry(s, s.controls, s.panel.options, given);
            updateHostDisplay(juce::AudioProcessor::ChangeDetails().withParameterInfoChanged(true));
            return;
        }
        if (!s.ok)
        {
            // The program that played plays on, unless a refine (Compiler.h, "The self-test") compiled it again since
            // this compile failed: a new program, the same files and dials (its carry their values as they are).
            if (!s.refined)
                return;
            Sources files;
            {
                const std::lock_guard<std::mutex> lock(programMutex_);
                if (s.program == 0 || s.program == program_)
                    return;
                program_ = s.program;
                files = running_;
                controls = controls_;
                options = panel_.options;
            }
            capture_.noteProgram(s.program, files, controls, s.circuits);
            if (run)
                writeCarry(s, controls, options, given);
            return;
        }
        capture_.noteProgram(s.program, s.sources, s.controls, s.circuits);   // a recording's replay compiles these
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            // A panel's edit of the program applied plays the same program: no run's. One of a program this has not
            // applied (poll() saw the edit's status, not its compile's) is that compile's too: its run replaced what
            // played.
            const bool edit = s.panelOnly && s.program == program_;
            controls_ = s.controls;
            panel_ = s.panel;
            program_ = s.program;
            // compare's previous_run: what a run replaced (its files, or the samples it read). A state's program (a
            // host's recall, a project reopened) is no run's: nothing played before it, though Init played until then.
            if (loaded)
            {
                previous_.clear();
                previousAssets_ = nullptr;
            }
            else if (!edit && changesProgram(s))
            {
                previous_ = running_;
                previousAssets_ = runningAssets_;
            }
            running_ = s.sources;
            runningAssets_ = s.assets;
            runningSamples_ = s.samples;
        }
        learn_.bind(s.controls);
        if (run)
            writeCarry(s, s.controls, s.panel.options, given);
        updateHostDisplay(juce::AudioProcessor::ChangeDetails().withParameterInfoChanged(true));
    }

    // The dials' carry (Controls.h, "a dial's value across a declaration change"): on the thread that offers the
    // program, just before (the compiler's, or compileNow's caller's).
    DialCarry Processor::carryFor(const Controls& controls, const PanelSpec& panel)
    {
        const std::lock_guard<std::mutex> lock(meaningMutex_);
        DialCarry k;
        k.ticket = ++carryTickets_;
        k.from = meaningProgram_.load(std::memory_order_relaxed);
        for (int c = 0; c < kMaxControls; ++c)
        {
            const auto i = static_cast<std::size_t>(c);
            const Control& to = controls[i];
            const float v = raw_[i]->load();
            if (!to.used || !std::isfinite(v))
                continue;                                        // (a host's NaN: the last good value plays on)
            const float norm = std::clamp(v, 0.0f, 1.0f);        // as readParameters gives it to the Engine
            const Control& from = meaningControls_[i];
            k.fromNorm[i] = norm;
            k.plain[i] = from.used && sameName(from, to)
                             ? carryValue(from, meaningOptions_[i], to, panel.options[i], plainOf(from, norm))
                             : to.def;                           // a parameter new to the slot: its default
            k.slots |= slotBit(c);
        }
        return k;
    }

    void Processor::writeCarry(const CompileStatus& s, const Controls& controls,
                               const std::array<std::vector<std::string>, kMaxControls>& options, const Given& given)
    {
        const DialCarry& k = s.carry;
        const std::lock_guard<std::mutex> lock(meaningMutex_);
        // A carry made since (a later program on its way, or played already) writes instead: this one's values came
        // from the same host values, which still mean what they meant.
        const bool newest = k.ticket != 0 && k.ticket == carryTickets_;
        // And each carry writes once: its status applied again (a new revision of the same program: the self-test's
        // notes) leaves a value moved back since (an undo, the host's automation) where it is.
        const bool carrying = newest && k.ticket != carryWritten_;
        for (int c = 0; c < kMaxControls; ++c)
        {
            const auto i = static_cast<std::size_t>(c);
            const Control& to = controls[i];
            const float v = raw_[i]->load();
            if (!to.used || !std::isfinite(v))
                continue;
            const float norm = std::clamp(v, 0.0f, 1.0f);
            float plain = 0.0f;
            if ((given.slots & slotBit(c)) != 0)
                plain = given.plain[i];
            else if (carrying && (k.slots & slotBit(c)) != 0 && norm == k.fromNorm[i])
                plain = k.plain[i];
            else
                continue;                                        // moved since (the host's, the user's): theirs stands
            if (plainOf(to, norm) == plain)
                continue;                                        // the host's value plays it already: no gesture
            ownDials_.fetch_or(slotBit(c));                      // the run's: no move (syncOwnDials)
            (void)writeDial(c, normOf(to, plain));
        }
        if (!newest)
            return;
        carryWritten_ = k.ticket;
        meaningControls_ = controls;
        meaningOptions_ = options;
        meaningProgram_.store(s.program, std::memory_order_release);   // after the writes (readParameters)
    }

    void Processor::adoptMeaning(const CompileStatus& s)
    {
        if (!s.ok)
            return;
        const std::lock_guard<std::mutex> lock(meaningMutex_);
        ++carryTickets_;                                         // a carry made before the load writes nothing now
        meaningControls_ = s.controls;
        meaningOptions_ = s.panel.options;
        meaningProgram_.store(s.program, std::memory_order_release);
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

    // run_function's "playing" (ChatAgent.h, Engine::Snapshot): asked of the audio thread, which copies at its next
    // block's end; taken back when no block comes in time (a host that has stopped the plugin's audio).
    bool Processor::chatSnapshot(Engine::Snapshot& s, std::string& why)
    {
        const auto waitFor = [&s](int ms) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (!s.done.load(std::memory_order_acquire))
            {
                if (std::chrono::steady_clock::now() >= until)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        };
        engine_.askSnapshot(&s);
        if (waitFor(kSnapshotWaitMs))
            return true;
        if (engine_.cancelSnapshot(&s))
        {
            why = "no audio went through the plugin for " + std::to_string(kSnapshotWaitMs) + " ms (its host has stopped "
                  "it, or nothing plays it), so nothing that plays could be copied";
            return false;
        }
        // Claimed as it was taken back: the copy ends within that block.
        if (waitFor(10000))
            return true;
        why = "the audio thread did not finish the copy";
        return false;
    }

    // The panel's fit (EditorPort.h's panelFitLine): laid out on this thread once the font is made, which is the message
    // thread's to make (poll() makes it once a program with a .gui file compiled): another thread waits for that a
    // little (a host polls 20 times a second), and says nothing when it is not made by then.
    void Processor::readyFit()
    {
        const PanelFitReady r = panelFitReady();
        fitChatWidth_.store(r.chatWidth);
        fitExplorerWidth_.store(r.explorerWidth);
        fitFonts_.store(r.fonts, std::memory_order_release);
    }

    std::string Processor::chatPanelFit(const PanelSpec& panel, const Controls& controls)
    {
        if (panel.gui.empty())
            return {};
        if (!fitFonts_.load(std::memory_order_acquire))
        {
            if (juce::MessageManager::existsAndIsCurrentThread())
                readyFit();
            else
                for (int k = 0; k < 50 && !fitFonts_.load(std::memory_order_acquire); ++k)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!fitFonts_.load(std::memory_order_acquire))
                return {};
        }
        // The explorer as the CODE view has it (it can narrow the chat, which the PANEL view keeps), none while the
        // code is in its own window.
        const int chat = chatW_.load(), explorer = explorerW_.load();
        return panelFitLine(panel, controls, editorW_.load(), editorH_.load(), chatOpen_.load(),
                            chat > 0 ? chat : fitChatWidth_.load(), explorerOpen_.load() && !codeWindowOpen(),
                            explorer > 0 ? explorer : fitExplorerWidth_.load());
    }

    std::string Processor::chatProject() const
    {
        const std::lock_guard<std::mutex> lock(programMutex_);
        return projectName_;
    }

    PanelPicture Processor::chatPicture(PictureView view, const std::string& name, const PictureState& state)
    {
        std::unique_lock<std::mutex> lock(pictureMutex_);
        pictureAsked_ = true;
        pictureReady_ = false;
        pictureView_ = view;
        picturePage_ = name;
        pictureState_ = state;
        // 10 s at most, and not past a stop: the message thread that draws it may be the one that stopped the turn and
        // waits for it to end (a project opened, a chat switched), which draws nothing until it has.
        using Clock = std::chrono::steady_clock;
        const Clock::time_point until = Clock::now() + std::chrono::seconds(10);
        while (!pictureReady_)
        {
            const bool stopped = chat_.stopping();
            if (stopped || Clock::now() >= until)
            {
                pictureAsked_ = false;
                PanelPicture none;
                none.why = stopped ? "stopped before the picture was drawn"
                                   : "the plugin's message thread did not draw the picture in time (the host may be busy); try again";
                return none;
            }
            pictureDone_.wait_until(lock, std::min(until, Clock::now() + std::chrono::milliseconds(20)));
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
                const PictureView view = pictureView_;
                const std::string page = picturePage_;
                const PictureState state = std::move(pictureState_);
                pictureState_ = {};
                lock.unlock();
                PanelPicture p = view == PictureView::schematic
                                     ? pictureSchematic(editorPort(), page)
                                     : picturePanel(ports(), editorPort(), editorW_.load(), editorH_.load(),
                                                    view == PictureView::window, page, state);
                lock.lock();
                picture_ = std::move(p);
                pictureReady_ = true;
                pictureDone_.notify_all();
            }
        }
        picturesIdle();                                          // the pictures' GPU context, let go when idle
        journalPoll();                                           // what moved since the last poll (ProcessorLive.cpp)
        abSessionLoaded();                                       // a session loaded since ends the A/B (ProcessorAb.cpp)
        if (compiler_.revision() != seen_)
        {
            const CompileStatus s = compiler_.status();
            seen_ = s.revision;
            // A version's program (or a preset's): its dials, by name, once it compiled (an older request's result
            // waits for its own); a fresh start's (an example's, RANDOM's): every dial at its default, as in a new
            // instance. Those slots take them in place of the carry, each written once.
            Given given;
            if (s.ok && dialsRequest_ != 0 && s.request == dialsRequest_)
                given = givenByName(s.controls, s.panel.options, projectDials_);
            else if (s.ok && defaultsRequest_ != 0 && s.request == defaultsRequest_)
            {
                for (int c = 0; c < kMaxControls; ++c)
                    if (const Control& k = s.controls[static_cast<std::size_t>(c)]; k.used)
                    {
                        given.plain[static_cast<std::size_t>(c)] = k.def;
                        given.slots |= slotBit(c);
                    }
            }
            else if (s.ok && abDialsRequest_ != 0 && s.request == abDialsRequest_)
                given = givenByName(s.controls, s.panel.options, abDials_);   // an A/B side's own (ProcessorAb.cpp)
            // A panel's edit of a program apply() has not taken (this poll missed its compile's status, Compiler.h) is
            // that compile's too: apply() writes its carry and moves previous_run, and its run is journalled as one.
            const bool edit = s.panelOnly && s.program == program();
            apply(s, true, false, given);
            abLanded(s);
            if (!s.panel.gui.empty() && !fitFonts_.load(std::memory_order_relaxed))
                readyFit();                                      // the panel's fit may be asked now (chatPanelFit)
            if (s.request == userRun_.load() && s.request != journaled_)
            {
                journaled_ = s.request;                          // a refine's status answers it again (Compiler.h)
                if (s.stopped)
                    stopJournaled_ = s.stoppedProgram;           // said with the run (runWords)
                if (s.ok && !s.panelOnly)
                    journal_.dialsNote(Journal::Who::user, runWords(s));   // a new dial at its default (apply)
                else if (s.ok && !edit)
                {
                    CompileStatus compiled = s;                  // (the compile it edits, whose run this answers too)
                    compiled.panelOnly = false;
                    journal_.dialsNote(Journal::Who::user, runWords(compiled));
                }
                else
                    journal_.note(Journal::Who::user, runWords(s));        // (a panel's edit sets no dial)
            }
            else if (s.stopped && (!s.restored.empty() || s.program == s.stoppedProgram) && s.stoppedProgram != stopJournaled_
                     && s.stoppedProgram != chatStopJournaled_.load())
            {
                // A program that stopped as it played, after its run was journalled (Compiler.h, "A program that stops").
                stopJournaled_ = s.stoppedProgram;
                const std::string first = s.errors.substr(0, s.errors.find('\n'));
                const std::size_t at = first.find(": error: ");
                journal_.note(Journal::Who::plugin, "the program stopped (" + (at == std::string::npos ? first : first.substr(at + 9))
                                                        + (s.restored.empty() ? "); nothing plays" : "); the program before plays again"));
            }
            if (dialsRequest_ != 0 && s.request >= dialsRequest_)
            {
                projectDials_.clear();
                dialsRequest_ = 0;
            }
            if (defaultsRequest_ != 0 && s.request >= defaultsRequest_)
            {
                if (s.ok && s.request == defaultsRequest_)
                    setDialsToDefaults(s.controls);              // the unused slots' too
                defaultsRequest_ = 0;
            }
            if (abDialsRequest_ != 0 && s.request >= abDialsRequest_)
            {
                abDials_.clear();
                abDialsRequest_ = 0;
            }
        }
        // The presets in turn: a program MIDI or the host asked for, the list read again, the host told of a change.
        if constexpr (product::kIsInstrument)
            if (const uint32_t m = midiProgram_.load(std::memory_order_acquire); (m >> 8) != (midiProgramSeen_ >> 8))
            {
                midiProgramSeen_ = m;
                (void)choosePreset(static_cast<int>(m & 0x7fu), "MIDI's program change");
            }
        if (const int asked = hostProgramAsked_.exchange(-1); asked >= 0)
            (void)choosePreset(asked, "the host");
        refreshPresets(false);
        {
            const int now = hostCurrent_.load();
            uint64_t names = 0;
            {
                const std::lock_guard<std::mutex> lock(presetsMutex_);
                names = heldNames_;
            }
            if (now != hostTold_ || names != namesTold_)
            {
                const bool first = hostTold_ < 0;
                hostTold_ = now;
                namesTold_ = names;
                if (!first)
                    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withProgramChanged(true));
            }
        }
        abMatched();                                             // the A/B's loudness, once measured
        abFollowDials();                                         // and again once its dials moved and came to rest
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
            ownDials_.fetch_or(slotBit(slot));                   // journalled when Claude set it (chatSetDial)
            (void)writeDial(slot, normOf(c, plain));
        }
        runClaudeJob();                                          // Claude's projects tool: after its dials, as it set them
        capture_.housekeep();                                    // frees what the audio thread handed back
        if ((learn_.listening() || midiGestures_ != 0) && !midiTimer_.isTimerRunning())
            midiTimer_.startTimerHz(kMidiHz);                    // MIDI learn's, while it has anything to do
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
        // The folder: a loaded state's first, then a pass (FolderSync.h).
        std::optional<FolderKept> wanted;
        {
            const std::lock_guard<std::mutex> lock(folderMutex_);
            wanted.swap(folderWanted_);
        }
        if (wanted && wanted->on)
        {
            folder_.setRunOnSave(wanted->run);
            std::string why;
            if (!folder_.on() || folder_.name() != wanted->name)
                (void)folder_.start(folderRoot(), wanted->name, seconds(), why);
        }
        folder_.pass(seconds());
        if (folder_.revision() != folderKeptAt_)
            keepFolder();
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
        std::vector<std::string> keys;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            program.setProperty(kActive, draft_.active, nullptr);
            if (!projectName_.empty())
            {
                program.setProperty(kProject, juce::String::fromUTF8(projectName_.c_str()), nullptr);
                if (!projectId_.empty())
                    program.setProperty(kProjectId, juce::String(projectId_), nullptr);
                program.setProperty(kProjectVersion, projectVersion_, nullptr);
                if (!projectPreset_.empty())
                    program.setProperty(kProjectPreset, juce::String::fromUTF8(projectPreset_.c_str()), nullptr);
            }
            // While an A/B is on (or its Close or Keep compiles), what played before it began (or the files on their
            // way), and those dials while each side has its own: a side never kept is not what the session plays.
            const Sources& run = abKept_.on ? abKept_.run : running_;
            program.appendChild(filesTree(kRunTag, run), nullptr);
            program.appendChild(filesTree(kDraftTag, draft_.files), nullptr);
            keys = circuits::keysOf(run);
            for (std::string& k : circuits::keysOf(draft_.files))
                keys.push_back(std::move(k));
            if (abKept_.on && abKept_.dials)
                for (int c = 0; c < kHostControls; ++c)
                {
                    juce::ValueTree p = state.getChildWithProperty(kParamId, juce::var(idOf(c)));
                    const float norm = abKept_.norms[static_cast<std::size_t>(c)];
                    if (p.isValid() && std::isfinite(norm))
                        p.setProperty(kParamValue, static_cast<double>(std::clamp(norm, 0.0f, 1.0f)), nullptr);
                }
            // The panel's own dials, every one beside the host's as the APVTS keeps every <PARAM>: a slot the program
            // that plays leaves unused may be one of the program the session keeps (an A/B's side with fewer dials
            // plays, its sides without their own dials); while an A/B keeps what played before it with its own dials,
            // each one's value then (its program's controls may be other than a side's).
            const bool abDials = abKept_.on && abKept_.dials;
            juce::ValueTree panel{ kPanelTag };
            for (int c = kHostControls; c < kMaxControls; ++c)
            {
                const auto i = static_cast<std::size_t>(c);
                const float kept = abDials ? abKept_.norms[i] : raw_[i]->load();
                const float norm = std::isfinite(kept) ? std::clamp(kept, 0.0f, 1.0f) : raw_[i]->load();
                juce::ValueTree dial{ kDialTag };
                dial.setProperty(kSlot, c, nullptr);
                dial.setProperty(kParamValue, static_cast<double>(norm), nullptr);
                panel.appendChild(dial, nullptr);
            }
            state.appendChild(panel, nullptr);
        }
        // The self-test's answers for the netlists of the program that plays and of the draft (Answers::of: each once).
        if (const std::vector<std::pair<std::string, std::string>> answers = circuits::answers().of(keys); !answers.empty())
        {
            juce::ValueTree tree{ kCircuitsTag };
            for (const auto& [key, text] : answers)
            {
                juce::ValueTree answer{ kAnswerTag };
                answer.setProperty(kKey, juce::String::fromUTF8(key.c_str()), nullptr);
                answer.setProperty(kText, juce::String::fromUTF8(text.c_str()), nullptr);
                tree.appendChild(answer, nullptr);
            }
            program.appendChild(tree, nullptr);
        }
        state.appendChild(program, nullptr);
        {
            // The chat stays on this Mac (written now, when it changed, unless a turn runs: its end writes it, and a
            // host may ask for the state at every change); the session names it only.
            chat_.keepIdle();
            const ChatAgent::ChatRef ref = chat_.chatRef();
            std::string instance;
            {
                const std::lock_guard<std::mutex> lock(programMutex_);
                instance = instanceId_;
            }
            juce::ValueTree chat{ kChatTag };
            chat.setProperty(kInstance, juce::String(instance), nullptr);
            chat.setProperty(kOwner, juce::String(ref.owner), nullptr);
            if (!ref.id.empty())
                chat.setProperty(kChatId, juce::String(ref.id), nullptr);
            state.appendChild(chat, nullptr);
        }
        // The user's name, kept even while the link has no socket (its name() is "" then, and a name restored into such
        // an instance must survive its next save).
        const std::string desktopName = mcp_ != nullptr ? mcp_->userName() : std::string();
        if (const std::vector<KeptMark> marks = chat_.mcpMarksToKeep(); !marks.empty() || !desktopName.empty())
        {
            const auto hex = [](uint64_t h) { return juce::String::toHexString(h).paddedLeft('0', 16); };
            juce::ValueTree mcp{ kMcpTag };
            if (!desktopName.empty())
                mcp.setProperty(kDesktopName, juce::String::fromUTF8(desktopName.c_str()), nullptr);
            for (const KeptMark& m : marks)
            {
                juce::ValueTree mark{ kMarkTag };
                mark.setProperty(kToken, juce::String(m.token), nullptr);
                for (const KeptMark::File& f : m.files)
                {
                    juce::ValueTree file{ kFileTag };
                    file.setProperty(kName, juce::String::fromUTF8(f.name.c_str()), nullptr);
                    file.setProperty(kHash, hex(f.hash), nullptr);
                    if (f.netlist != 0)
                        file.setProperty(kNetlist, hex(f.netlist), nullptr);
                    file.setProperty(kLines, f.lines, nullptr);
                    mark.appendChild(file, nullptr);
                }
                mcp.appendChild(mark, nullptr);
            }
            state.appendChild(mcp, nullptr);
        }
        const std::vector<std::string> pages = guiPages();
        const std::vector<FileView> views = fileViews();
        const CodeWindow code = codeWindow();
        if ((editorW_.load() > 0 && editorH_.load() > 0) || panelView_.load() || codePanelHidden_.load()
            || guiPanelHidden_.load() || !chatOpen_.load() || !explorerOpen_.load() || explorerW_.load() > 0
            || chatW_.load() > 0 || !pages.empty() || !views.empty() || !(code == CodeWindow{}))
        {
            juce::ValueTree editor{ kEditorTag };
            if (editorW_.load() > 0 && editorH_.load() > 0)
            {
                editor.setProperty(kWidth, editorW_.load(), nullptr);
                editor.setProperty(kHeight, editorH_.load(), nullptr);
            }
            if (panelView_.load())
                editor.setProperty(kView, "panel", nullptr);
            if (codePanelHidden_.load())
                editor.setProperty(kCodePanel, "hidden", nullptr);
            if (guiPanelHidden_.load())
                editor.setProperty(kGuiPanel, "hidden", nullptr);
            if (!chatOpen_.load())
                editor.setProperty(kChat, "closed", nullptr);
            if (!explorerOpen_.load())
                editor.setProperty(kExplorer, "closed", nullptr);
            if (explorerW_.load() > 0)
                editor.setProperty(kExplorerW, explorerW_.load(), nullptr);
            if (chatW_.load() > 0)
                editor.setProperty(kChatW, chatW_.load(), nullptr);
            if (code.out)
                editor.setProperty(kCodeWindow, "out", nullptr);
            if (code.width > 0 && code.height > 0)
            {
                editor.setProperty(kCodeW, code.width, nullptr);
                editor.setProperty(kCodeH, code.height, nullptr);
            }
            if (code.placed)
            {
                editor.setProperty(kCodeX, code.x, nullptr);
                editor.setProperty(kCodeY, code.y, nullptr);
            }
            for (const std::string& title : pages)
            {
                juce::ValueTree page{ kPageTag };
                page.setProperty(kTitle, juce::String::fromUTF8(title.c_str()), nullptr);
                editor.appendChild(page, nullptr);
            }
            for (const FileView& v : views)
            {
                juce::ValueTree file{ kFileViewTag };
                file.setProperty(kName, juce::String::fromUTF8(v.file.c_str()), nullptr);
                file.setProperty(kView, v.schematic ? "schematic" : "netlist", nullptr);
                file.setProperty(kZoom, static_cast<double>(v.zoom), nullptr);
                file.setProperty(kX, static_cast<double>(v.x), nullptr);
                file.setProperty(kY, static_cast<double>(v.y), nullptr);
                for (const std::string& node : v.probes)
                {
                    juce::ValueTree probe{ kProbeTag };
                    probe.setProperty(kNode, juce::String::fromUTF8(node.c_str()), nullptr);
                    file.appendChild(probe, nullptr);
                }
                editor.appendChild(file, nullptr);
            }
            state.appendChild(editor, nullptr);
        }
        {
            const std::lock_guard<std::mutex> lock(folderMutex_);
            if (folderKept_.on)
            {
                juce::ValueTree folder{ kFolderTag };
                folder.setProperty(kName, juce::String::fromUTF8(folderKept_.name.c_str()), nullptr);
                if (folderKept_.run)
                    folder.setProperty(kRunOnSave, 1, nullptr);
                state.appendChild(folder, nullptr);
            }
        }
        if (const std::vector<midilearn::Mapping> maps = learn_.mappings(); !maps.empty())
        {
            juce::ValueTree midi{ kMidiTag };
            for (const midilearn::Mapping& m : maps)
            {
                juce::ValueTree map{ kMapTag };
                map.setProperty(kDial, juce::String::fromUTF8(m.dial.c_str()), nullptr);
                map.setProperty(kChannel, m.channel, nullptr);
                map.setProperty(kCc, m.cc, nullptr);
                if (m.fine)
                    map.setProperty(kFine, 1, nullptr);
                midi.appendChild(map, nullptr);
            }
            state.appendChild(midi, nullptr);
        }
        // The samples, each file as it was added (ProcessorAssets.cpp).
        if (juce::ValueTree samples = assetsState(); samples.getNumChildren() > 0)
            state.appendChild(samples, nullptr);
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
        for (int c = 0; c < kHostControls; ++c)
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
        // The panel's own dials (a state from before them has none: each at its default, below).
        std::array<float, kPanelControls> panelKept;
        panelKept.fill(std::numeric_limits<float>::quiet_NaN());
        if (const juce::ValueTree panel = state.getChildWithName(kPanelTag); panel.isValid())
        {
            for (int k = 0; k < panel.getNumChildren() && k < 2 * kPanelControls; ++k)
                if (const juce::ValueTree dial = panel.getChild(k); dial.hasType(kDialTag))
                {
                    const int c = static_cast<int>(dial.getProperty(kSlot, -1));
                    const double v = static_cast<double>(dial.getProperty(kParamValue, 0.5));
                    if (isPanelDial(c))
                        panelKept[static_cast<std::size_t>(c - kHostControls)] =
                            std::isfinite(v) ? std::clamp(static_cast<float>(v), 0.0f, 1.0f) : 0.5f;   // as a PARAM's
                }
            state.removeChild(panel, nullptr);
        }
        // The chat: which one (applied once the project open is known, below), or a conversation from before.
        const juce::ValueTree chat = state.getChildWithName(kChatTag);
        if (chat.isValid())
            state.removeChild(chat, nullptr);
        {
            // MCP's marks, now of before the plugin was opened again (none in a state from before them).
            const juce::ValueTree mcp = state.getChildWithName(kMcpTag);
            const auto hash = [](const juce::var& v) { return static_cast<uint64_t>(v.toString().getHexValue64()); };
            std::vector<KeptMark> marks;
            for (int k = 0; mcp.isValid() && k < mcp.getNumChildren() && k < 64; ++k)
                if (const juce::ValueTree mark = mcp.getChild(k); mark.hasType(kMarkTag))
                {
                    KeptMark m;
                    m.token = mark.getProperty(kToken).toString().substring(0, 64).toStdString();
                    for (int j = 0; j < mark.getNumChildren() && j < 256; ++j)
                        if (const juce::ValueTree file = mark.getChild(j); file.hasType(kFileTag))
                            m.files.push_back({ file.getProperty(kName).toString().substring(0, 200).toStdString(),
                                                hash(file.getProperty(kHash)), hash(file.getProperty(kNetlist, "0")),
                                                std::max(0, static_cast<int>(file.getProperty(kLines, 0))) });
                    marks.push_back(std::move(m));
                }
            chat_.restoreMcpMarks(std::move(marks));
            if (mcp.isValid() && mcp.hasProperty(kDesktopName) && mcp_ != nullptr)
            {
                std::string why;
                (void)mcp_->setName(mcp.getProperty(kDesktopName).toString().toStdString(), why);   // one it cannot take: as it was
            }
            if (mcp.isValid())
                state.removeChild(mcp, nullptr);
        }
        // The folder, for poll() to start on the message thread (a state without one leaves it as it is: a preset or a
        // session from before folders knows nothing of it, and the folder follows the files it brings).
        if (const juce::ValueTree folder = state.getChildWithName(kFolderTag); folder.isValid())
        {
            FolderKept k;
            k.on = true;
            k.name = FolderSync::tidyFolderName(folder.getProperty(kName).toString().substring(0, 200).toStdString());
            k.run = static_cast<int>(folder.getProperty(kRunOnSave, 0)) != 0;
            if (!k.name.empty())
            {
                const std::lock_guard<std::mutex> lock(folderMutex_);
                folderWanted_ = k;
            }
            state.removeChild(folder, nullptr);
        }
        // MIDI learn's mappings (a state from before them, or saved with none, has none), bound when its program applies.
        {
            std::vector<midilearn::Mapping> maps;
            const juce::ValueTree midi = state.getChildWithName(kMidiTag);
            for (int k = 0; midi.isValid() && k < midi.getNumChildren() && k < 4 * kMaxControls; ++k)
                if (const juce::ValueTree map = midi.getChild(k); map.hasType(kMapTag))
                    maps.push_back({ map.getProperty(kDial).toString().substring(0, 64).toStdString(),
                                     static_cast<int>(map.getProperty(kChannel, 0)), static_cast<int>(map.getProperty(kCc, -1)),
                                     static_cast<int>(map.getProperty(kFine, 0)) != 0 });
            learn_.setMappings(std::move(maps));
            if (midi.isValid())
                state.removeChild(midi, nullptr);
        }
        const juce::ValueTree editor = state.getChildWithName(kEditorTag);
        panelView_.store(editor.isValid() && editor.getProperty(kView).toString() == "panel");
        // The CODE view's panel (a state from before the choice has it shown under every file).
        codePanelHidden_.store(editor.isValid() && editor.getProperty(kCodePanel).toString() == "hidden");
        guiPanelHidden_.store(editor.isValid() && editor.getProperty(kGuiPanel).toString() == "hidden");
        chatOpen_.store(!(editor.isValid() && editor.getProperty(kChat).toString() == "closed"));
        explorerOpen_.store(!(editor.isValid() && editor.getProperty(kExplorer).toString() == "closed"));
        // The widths its user set (a state from before them has none: the editor's default).
        explorerW_.store(editor.isValid() ? std::clamp(static_cast<int>(editor.getProperty(kExplorerW, 0)), 0, kMaxEditorSide) : 0);
        chatW_.store(editor.isValid() ? std::clamp(static_cast<int>(editor.getProperty(kChatW, 0)), 0, kMaxEditorSide) : 0);
        // The code window (a state from before it has the editor in place, the window at its default size and place).
        {
            CodeWindow code;
            if (editor.isValid())
            {
                code.out = editor.getProperty(kCodeWindow).toString() == "out";
                code.width = static_cast<int>(editor.getProperty(kCodeW, 0));
                code.height = static_cast<int>(editor.getProperty(kCodeH, 0));
                code.placed = editor.hasProperty(kCodeX) && editor.hasProperty(kCodeY);
                code.x = static_cast<int>(editor.getProperty(kCodeX, 0));
                code.y = static_cast<int>(editor.getProperty(kCodeY, 0));
            }
            setCodeWindow(code);
        }
        {
            std::vector<std::string> pages;
            for (int k = 0; editor.isValid() && k < editor.getNumChildren() && k < 64; ++k)
                if (const juce::ValueTree page = editor.getChild(k); page.hasType(kPageTag))
                    pages.push_back(page.getProperty(kTitle).toString().substring(0, 200).toStdString());
            setGuiPages(pages);
            // The netlists' views (a state from before them has none): a file's name, its view, zoom and centre.
            std::vector<FileView> views;
            for (int k = 0; editor.isValid() && k < editor.getNumChildren() && k < 64; ++k)
                if (const juce::ValueTree file = editor.getChild(k); file.hasType(kFileViewTag))
                {
                    FileView v;
                    v.file = file.getProperty(kName).toString().substring(0, 200).toStdString();
                    v.schematic = file.getProperty(kView).toString() == "schematic";
                    v.zoom = static_cast<float>(static_cast<double>(file.getProperty(kZoom, 1.0)));
                    v.x = static_cast<float>(static_cast<double>(file.getProperty(kX, 0.0)));
                    v.y = static_cast<float>(static_cast<double>(file.getProperty(kY, 0.0)));
                    for (int j = 0; j < file.getNumChildren() && v.probes.size() < kMaxFileProbes; ++j)
                        if (const juce::ValueTree probe = file.getChild(j); probe.hasType(kProbeTag))
                            v.probes.push_back(probe.getProperty(kNode).toString().substring(0, 200).toStdString());
                    views.push_back(std::move(v));
                }
            setFileViews(views);
        }
        if (editor.isValid())
        {
            setEditorSize(static_cast<int>(editor.getProperty(kWidth, 0)), static_cast<int>(editor.getProperty(kHeight, 0)));
            state.removeChild(editor, nullptr);
        }
        // The samples (a state from before them, or saved with none, has none), decoded here, before its program
        // compiles with them.
        std::vector<std::string> samplesRefused;
        const Assets samples = assetsOfState(state, samplesRefused);
        state.removeChild(state.getChildWithName("ASSETS"), nullptr);
        replaceAssets(samples, Journal::Who::host, "");
        if (!samplesRefused.empty())
            journal_.note(Journal::Who::host, "left out of the saved state's samples: " + samplesRefused.front()
                                                  + (samplesRefused.size() > 1 ? " (and " + std::to_string(samplesRefused.size() - 1) + " more)" : ""));
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            projectAssets_ = samples;                            // the session's samples are its version's, as saved
            // The project open; a session from before projects names a preset, now a project of that name.
            if (program.hasProperty(kProject))
            {
                // A factory project's name as it is (tidyName would take its colon), while this build has it.
                const std::string named = program.getProperty(kProject).toString().toStdString();
                projectName_ = !isFactoryName(named)                              ? projectstore::tidyName(named)
                             : factory::find(named) != nullptr ? factory::find(named)->name
                                                               : std::string();
                projectVersion_ = std::max(0, static_cast<int>(program.getProperty(kProjectVersion, 0)));
                projectPreset_ = projectstore::tidyName(program.getProperty(kProjectPreset).toString().toStdString());
                projectId_ = program.getProperty(kProjectId).toString().toLowerCase().substring(0, 64).toStdString();
                projectIdName_.clear();                          // checked against the project's file (chatOwnerNow)
            }
            else
            {
                projectName_ = projectstore::tidyName(program.getProperty(kPreset).toString().toStdString());
                projectVersion_ = 0;
                projectPreset_.clear();
                projectId_.clear();
                projectIdName_.clear();
            }
            projectLatest_ = projectVersion_;
            projectFiles_ = draft.files;                         // the session's files are its version's, as saved
            baseDials_.clear();                                  // restoreProjectBase's, below
            midiOpened_.reset();                                 // the session's mappings are its own
            draft_ = std::move(draft);
            ++draftRevision_;
            abKept_ = {};                                        // an A/B's: the session's program plays, as saved
        }
        restoreProjectBase();
        // An A/B (ProcessorAb.cpp) ends: its compile still waiting is taken back (it would play over the session's
        // program), the session's program plays untrimmed from its first block, and poll() ends it once that is out.
        abLoading_.store(true);
        (void)compiler_.withdraw(abQueued_.load());
        // The self-test's answers it kept (each read back as this FMinus's answer for its key, or left out), stored
        // before it compiles: the program plays its refined configuration from the first block.
        const juce::ValueTree kept = program.getChildWithName(kCircuitsTag);
        for (int k = 0; kept.isValid() && k < kept.getNumChildren() && k < static_cast<int>(circuits::Answers::kCapacity); ++k)
            if (const juce::ValueTree a = kept.getChild(k); a.hasType(kAnswerTag))
                circuits::restore(a.getProperty(kKey).toString().toStdString(), a.getProperty(kText).toString().toStdString());
        // The session's program, compiled now, its dials as the session has them.
        const CompileStatus s = compiler_.compileNow(std::move(run), false);   // its dials come with it: no carry
        seen_ = s.revision;
        apply(s, false, true, Given{});
        apvts_.replaceState(state);
        for (int k = 0; k < kPanelControls; ++k)
        {
            // Not a gesture (writeDial): the values come with the session, as the host's parameters' do.
            const int c = kHostControls + k;
            const Control& d = s.controls[static_cast<std::size_t>(c)];
            const float saved = panelKept[static_cast<std::size_t>(k)];
            panelValues_[static_cast<std::size_t>(k)].store(std::isfinite(saved) ? saved : d.used ? normOf(d, d.def) : dialDefault01(c));
        }
        adoptMeaning(s);                                         // the values are the session's program's now
        journal_.dialsNote(Journal::Who::host, "loaded a saved state (the program, its files and its dials)");
        syncDials();
        abStateLoaded_.store(std::max<uint64_t>(1, s.request));
        // The session's project's presets (restoreProjectBase read them into the cache), its preset the host's program.
        refreshPresets(true);

        // The chat (ChatAgent.h, "The chats"): this instance's id (its chats while no project is open), then the project
        // open's chat the session names, else that project's newest; a session from before keeps its conversation on this
        // Mac now; a state with no chat at all (a preset from before chats) shows the project's chats all the same.
        loadChat(chat);
    }

    // The chat a state names (setStateInformation). The instance's id: the state's own when it is this instance's, or
    // when its chats are there to find and no other live instance holds it; a new one when another does (a track
    // duplicated, a default preset applied: the chat the state names is shown as a copy of this instance's own), and
    // when there is nothing of it to find (a template's or a default preset's from an instance that kept no chat, a
    // state from another Mac): so copies of one state never share chats. A session from before has no id: one made from
    // its conversation when that is adoptable and kept with the instance (loaded again unsaved, the same instance finds
    // the chat it became), else a new one (every untouched instance's conversation was alike).
    void Processor::loadChat(const juce::ValueTree& chat)
    {
        if (!chat.isValid())
        {
            std::string owner, label;
            chatOwnerNow(owner, label);
            chat_.setOwner(owner, label, ChatAgent::OwnerChange::open);
            return;
        }
        const bool old = chat.hasProperty(kJson);
        const std::string json = old ? chat.getProperty(kJson).toString().toStdString() : std::string();
        const std::string sessionOwner = chat.getProperty(kOwner).toString().toStdString();
        const std::string sessionChat = chat.getProperty(kChatId).toString().toStdString();
        std::string mine;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            mine = instanceId_;
        }
        // The project's chats first (the owner when one is open): the instance's id matters only without one.
        std::string owner, label;
        chatOwnerNow(owner, label);
        const bool project = owner.rfind(chatstore::kInstancePrefix, 0) != 0;
        std::string wanted = old ? std::string() : chat.getProperty(kInstance).toString().toStdString();
        if (old && !project && ChatAgent::adoptable(json))
            wanted = ChatAgent::sessionChatId(json);
        if (!chatstore::validId(wanted))
            wanted.clear();
        else if (wanted != mine && !old && !chatstore::hasChats(chatstore::instanceOwner(wanted)))
            wanted.clear();                                      // nothing of it here to find
        std::string copyFrom;                                     // the instance whose chat this one shows a copy of
        std::string next = wanted.empty() ? chatstore::newId() : wanted;
        if (!takeId(mine, next))
        {
            if (!project && sessionOwner == chatstore::instanceOwner(next))
                copyFrom = sessionOwner;
            next = chatstore::newId();
            (void)takeId(mine, next);
        }
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            instanceId_ = next;
        }
        if (!project)
            owner = chatstore::instanceOwner(next);
        if (old)
            chat_.adoptChat(json, owner, label);
        else if (!copyFrom.empty())
            chat_.restoreChat(owner, label, sessionChat, copyFrom);
        else
            chat_.restoreChat(owner, label, sessionOwner == owner ? sessionChat : std::string());
    }

    // The chats' owner now (ChatAgent.h, "The chats"): the project open's id (made now when its file has none: a project
    // from before ids; found by the id the session kept when the project was renamed in the Finder since), its name the
    // label; else this instance's own. projectId_ follows.
    void Processor::chatOwnerNow(std::string& owner, std::string& label, const projectstore::Project* opened)
    {
        std::string name, kept, keptFor, instance;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            name = projectName_;
            kept = projectId_;
            keptFor = projectIdName_;
            instance = instanceId_;
        }
        std::string id;
        if (!name.empty() && !kept.empty() && projectstore::sameName(name, keptFor) && opened == nullptr)
            id = kept;                                           // the same project as before (a version saved): no read
        else if (opened != nullptr && projectstore::sameName(opened->name, name))
            id = projectstore::idOf(*opened);                    // read already: not read again
        else if (!name.empty())
        {
            id = projectstore::idOf(name);
            if (id.empty() && projectstore::validProjectId(kept))
                if (const std::string renamed = projectstore::nameOfId(kept); !renamed.empty())
                {
                    name = renamed;                              // renamed in the Finder since the session named it
                    id = projectstore::idOf(name);
                }
        }
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            if (!id.empty() && !projectstore::sameName(projectName_, name))
                projectName_ = name;
            projectId_ = id;
            projectIdName_ = id.empty() ? std::string() : name;
        }
        owner = id.empty() ? chatstore::instanceOwner(instance) : id;
        label = id.empty() ? std::string() : name;
    }

    void Processor::syncChatOwner(ChatAgent::OwnerChange how, const projectstore::Project* opened)
    {
        std::string owner, label;
        chatOwnerNow(owner, label, opened);
        // A save carries the chat from a project that has no file any more (deleted since it was opened: the save made a
        // new project of its name): it moves, as nothing is left with that project. One renamed away keeps its own.
        if (how == ChatAgent::OwnerChange::carry)
            if (const std::string from = chat_.chatOwner(); from != owner && projectstore::validProjectId(from)
                                                             && projectstore::nameOfId(from).empty())
                how = ChatAgent::OwnerChange::move;
        chat_.setOwner(owner, label, how);
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
        journalDraft(before, draft, Journal::Who::user);
    }

    bool Processor::setDraftIf(const Draft& draft, uint64_t revision)
    {
        Draft before;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            if (draftRevision_ != revision)
                return false;                            // Claude's write (or a host's load) the editor has not merged
            before = std::exchange(draft_, draft);
        }
        journalDraft(before, draft, Journal::Who::user);
        return true;
    }

    void Processor::run(const Sources& files)
    {
        const uint64_t id = compiler_.request(files, panelEdits());
        userRun_.store(id);
    }

    // A RUN may be a panel's edit (Compiler.h, "A panel's edit") unless an A/B is on, whose sides are compiles of their
    // own (any thread: chatAb's line, "" while there is none).
    bool Processor::panelEdits() const { return chatAb().empty(); }

    bool Processor::compiling() const { return compiler_.busy(); }
    CompileStatus Processor::status() const { return compiler_.status(); }
    uint64_t Processor::statusRevision() const { return compiler_.revision(); }
    void Processor::check(const Sources& files) { compiler_.check(files); }
    CheckStatus Processor::checkStatus() const { return compiler_.checkStatus(); }

    // The probes' owners (EditorPort.h): the newest with a set armed reaches the Engine, only when that changes.
    int Processor::addProbeOwner()
    {
        probeOwners_.push_back({ ++probeOwnersMade_, ProbeSet{} });
        return probeOwnersMade_;
    }

    void Processor::setProbes(int owner, const ProbeSet& set)
    {
        for (ProbeOwner& o : probeOwners_)
            if (o.id == owner)
                o.set = set;
        sendProbes();
    }

    void Processor::removeProbeOwner(int owner)
    {
        probeOwners_.erase(std::remove_if(probeOwners_.begin(), probeOwners_.end(), [owner](const ProbeOwner& o) { return o.id == owner; }),
                           probeOwners_.end());
        sendProbes();
    }

    void Processor::sendProbes()
    {
        ProbeSet set;
        for (auto o = probeOwners_.rbegin(); o != probeOwners_.rend(); ++o)
            if (o->set.armed())
            {
                set = o->set;
                break;
            }
        if (probesSentOnce_ && set == probesSent_)
            return;
        probesSent_ = set;
        probesSentOnce_ = true;
        engine_.setProbes(set);
    }

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
                {
                    it->value = d.value;
                    it->label = d.label;
                }
                else
                    over.push_back(d);
            }
            return over;
        }
    }

    std::vector<ProjectEntry> Processor::projects() const
    {
        return entriesOf(projectstore::list());
    }

    std::vector<ProjectEntry> Processor::factoryProjects() const
    {
        std::vector<ProjectEntry> out;
        for (const projectstore::Entry& e : projectstore::factoryList())
            out.push_back({ e.name, whenOf(e.savedMs), e.latest, e.versions, static_cast<int>(e.presets.size()), true,
                            e.category, e.tags, e.description, e.savedMs });
        return out;
    }

    std::vector<ProjectEntry> Processor::projectsPolled() const
    {
        std::vector<std::string> unread;
        std::vector<ProjectEntry> out = entriesOf(projectstore::listCached(unread));
        if (!unread.empty())
            reader_->request(unread);
        return out;
    }

    std::vector<PresetEntry> Processor::presetsPolled(const std::string& project) const
    {
        std::vector<std::string> unread;
        std::vector<PresetEntry> out;
        if (const auto ps = projectstore::presetsCached(project, unread))
            for (const projectstore::PresetSummary& pr : *ps)
                out.push_back({ pr.name, whenOf(pr.savedMs), pr.from, pr.category });
        if (!unread.empty())
            reader_->request(unread);
        return out;
    }

    uint64_t Processor::projectsRead() const { return reader_->revision(); }

    std::vector<ProjectEntry> Processor::entriesOf(const std::vector<projectstore::Entry>& list) const
    {
        std::vector<ProjectEntry> out;
        for (const projectstore::Entry& e : list)
            out.push_back({ e.name, whenOf(e.savedMs), e.latest, e.versions, static_cast<int>(e.presets.size()), false,
                            e.category, e.tags, e.description, e.savedMs });
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
        return (path.startsWith(home + juce::File::getSeparatorString()) ? "~" + path.substring(home.length()) : path)
            .toStdString();
    }

    ProjectState Processor::project() const
    {
        ProjectState st;
        std::vector<projectstore::Dial> base;
        Controls cs;
        std::array<std::vector<std::string>, kMaxControls> options;
        const Assets samples = assetsNow();
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            options = panel_.options;
            st.name = projectName_;
            st.version = projectVersion_;
            st.latest = std::max(projectLatest_, projectVersion_);
            st.preset = projectPreset_;
            st.changed = draft_.files != projectFiles_ || !assets::same(samples.get(), projectAssets_.get());
            st.edited = !projectName_.empty() && st.changed;
            base = baseDials_;
            cs = controls_;
        }
        // The dials against the ones the version or the preset set (not while its compile is on its way).
        if (dialsRequest_ == 0)
            st.dialsEdited = !dialsAgainst(cs, options, base).second;
        return st;
    }

    std::pair<int, bool> Processor::dialsAgainst(const Controls& cs, const std::array<std::vector<std::string>, kMaxControls>& options,
                                                 const std::vector<projectstore::Dial>& dials) const
    {
        int found = 0;
        for (int c = 0; c < kMaxControls; ++c)
        {
            const Control& k = cs[static_cast<std::size_t>(c)];
            if (!k.used)
                continue;
            const auto it = std::find_if(dials.begin(), dials.end(), [&](const projectstore::Dial& d) { return d.name == k.name; });
            if (it == dials.end())
                continue;
            ++found;
            const float norm = raw_[static_cast<std::size_t>(c)]->load();
            const float plain = std::isfinite(norm) ? plainOf(k, norm) : k.def;
            const float saved = projectstore::valueOf(*it, k, options[static_cast<std::size_t>(c)]);
            if (std::fabs(plain - saved) > 1.0e-4f * std::max(1.0e-6f, std::fabs(k.max - k.min)))
                return { found, false };
        }
        return { found, true };
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
        if (const auto ps = projectstore::presetsOf(project))   // its file read only when it changed
            for (const projectstore::PresetSummary& pr : *ps)
                out.push_back({ pr.name, whenOf(pr.savedMs), pr.from, pr.category });
        return out;
    }

    std::vector<projectstore::Dial> Processor::dialsNow() const
    {
        Controls cs;
        std::array<std::vector<std::string>, kMaxControls> options;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            cs = controls_;
            options = panel_.options;
        }
        std::vector<projectstore::Dial> out;
        for (int c = 0; c < kMaxControls; ++c)
            if (const Control& k = cs[static_cast<std::size_t>(c)]; k.used)
            {
                const float norm = raw_[static_cast<std::size_t>(c)]->load();
                const float plain = std::isfinite(norm) ? plainOf(k, norm) : k.def;
                out.push_back({ k.name, plain, optionAt(k, plain, options[static_cast<std::size_t>(c)]) });
            }
        return out;
    }

    Processor::Given Processor::givenByName(const Controls& cs, const std::array<std::vector<std::string>, kMaxControls>& options,
                                            const std::vector<projectstore::Dial>& dials)
    {
        Given out;
        for (const projectstore::Dial& d : dials)
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = cs[static_cast<std::size_t>(c)];
                if (!k.used || d.name != k.name)
                    continue;
                out.plain[static_cast<std::size_t>(c)] = projectstore::valueOf(d, k, options[static_cast<std::size_t>(c)]);
                out.slots |= slotBit(c);
                break;
            }
        return out;
    }

    // Host gestures, on the message thread: each dial on the parameter of its name (a name the program lacks: none),
    // by its option's name first; a dial already there is not written.
    void Processor::setDialsByName(const Controls& cs, const std::array<std::vector<std::string>, kMaxControls>& options,
                                   const std::vector<projectstore::Dial>& dials)
    {
        const Given given = givenByName(cs, options, dials);
        for (int c = 0; c < kMaxControls; ++c)
        {
            if ((given.slots & slotBit(c)) == 0)
                continue;
            const Control& k = cs[static_cast<std::size_t>(c)];
            const float to = normOf(k, given.plain[static_cast<std::size_t>(c)]);
            if (raw_[static_cast<std::size_t>(c)]->load() == to)
                continue;
            ownDials_.fetch_or(slotBit(c));                      // a version's or a preset's: its line says so
            (void)writeDial(c, to);
        }
    }

    // Every dial at its default: a used slot's the program's fminus::default, an unused one the parameter's own (as a
    // new instance has them). Only those that are elsewhere move, each a host gesture.
    void Processor::setDialsToDefaults(const Controls& cs)
    {
        for (int c = 0; c < kMaxControls; ++c)
        {
            const Control& k = cs[static_cast<std::size_t>(c)];
            const float to = k.used ? normOf(k, k.def) : dialDefault01(c);
            if (raw_[static_cast<std::size_t>(c)]->load() == to)
                continue;
            ownDials_.fetch_or(slotBit(c));                      // the example's: its line says so
            (void)writeDial(c, to);
        }
    }

    bool Processor::writeDial(int c, float norm)
    {
        if (c < 0 || c >= kMaxControls || !std::isfinite(norm))
            return false;
        norm = std::clamp(norm, 0.0f, 1.0f);
        if (isPanelDial(c))
        {
            std::atomic<float>& v = panelValues_[static_cast<std::size_t>(c - kHostControls)];
            if (v.load() == norm)
                return false;
            v.store(norm);
            // No parameter moved, but the session did: a host that saves on change (VST3's dirty flag) is told.
            updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
            return true;
        }
        // A gesture of its own, through the dial's port: nested in one the editor's hand or a MIDI touch holds open (the
        // host sees that one go on, never ended under it, and its own end then the last), and not the editor's move in
        // the journal (GesturePort::beginOwnGesture).
        juce::RangedAudioParameter& p = *params_[static_cast<std::size_t>(c)];
        GesturePort& port = *portStore_[static_cast<std::size_t>(c)];
        const bool moved = p.getValue() != norm;
        port.beginOwnGesture();
        p.setValueNotifyingHost(norm);
        port.endGesture();
        return moved;
    }

    // ---- MIDI learn (MidiLearn.h) ------------------------------------------------------------------------------------
    bool Processor::midiLearnable() const { return product::kIsInstrument; }

    // The Standalone's racing wheel on or not (MidiLearn.h: its controls on channel 1 are not learned while it is), read
    // from its devices when they say something changed; none in a host. Message thread (the devices are its alone).
    void Processor::syncWheel()
    {
        DevicePort* d = windowHooks_.devices;
        if (d == nullptr)
        {
            learn_.setWheel(false);
            return;
        }
        const uint64_t serial = d->serial();
        if (wheelKnown_ && serial == wheelSerial_)
            return;
        wheelKnown_ = true;
        wheelSerial_ = serial;
        const DeviceSetup s = d->setup();
        learn_.setWheel(std::any_of(s.midi.begin(), s.midi.end(),
                                    [](const MidiInputDevice& m) { return m.on && m.identifier == wheel::kIdentifier; }));
    }

    void Processor::pollMidi(double nowMs)
    {
        if (learn_.learning())
            syncWheel();                                         // switched on or off while a dial learns
        midilearn::Learn::Tick t;
        learn_.tick(nowMs, t);
        if (t.learned)
            journal_.note(Journal::Who::user, "learned MIDI " + midilearn::describe(t.mapping) + " for " + t.mapping.dial);
        // The touches that ended close their gestures first, so a slot's next touch is a gesture of its own; then the
        // moves, through the dial's port as the editor's hand writes (an int's or a bool's on one of its values), a
        // gesture beginning with the first that moves it.
        for (int c = 0; c < kMaxControls; ++c)
            if ((t.ended & midiGestures_ & slotBit(c)) != 0)
            {
                ports_[static_cast<std::size_t>(c)]->endGesture();
                midiGestures_ &= ~slotBit(c);
            }
        for (const midilearn::Learn::Move& m : t.moves)
        {
            const Control k = control(m.slot);
            if (!k.used || !std::isfinite(m.value))
                continue;
            const float v = std::clamp(m.value, 0.0f, 1.0f);
            const float to = k.type == ControlType::real ? v : normOf(k, plainOf(k, v));
            funkgui::ParamPort& port = *ports_[static_cast<std::size_t>(m.slot)];
            if (port.value01() == to)
                continue;
            if ((midiGestures_ & slotBit(m.slot)) == 0)
            {
                port.beginGesture();
                midiGestures_ |= slotBit(m.slot);
            }
            port.setValue01(to);
        }
        if (!learn_.listening() && midiGestures_ == 0 && learn_.touching() == 0)
            midiTimer_.stopTimer();                              // poll() starts it again when there is something
    }

    void Processor::endMidiGestures()
    {
        for (int c = 0; c < kMaxControls; ++c)
            if ((midiGestures_ & slotBit(c)) != 0)
                ports_[static_cast<std::size_t>(c)]->endGesture();
        midiGestures_ = 0;
    }

    bool Processor::learnMidi(int slot)
    {
        if (!midiLearnable())
            return false;
        syncWheel();
        if (!learn_.arm(slot))
            return false;
        if (!midiTimer_.isTimerRunning())
            midiTimer_.startTimerHz(kMidiHz);
        return true;
    }

    void Processor::cancelMidiLearn() { learn_.cancel(); }

    bool Processor::forgetMidi(int slot)
    {
        const midilearn::View v = learn_.view();
        const std::string words = slot >= 0 && slot < kMaxControls ? v.words[static_cast<std::size_t>(slot)] : std::string();
        if (!learn_.forget(slot))
            return false;
        journal_.note(Journal::Who::user, "forgot the MIDI control of " + std::string(control(slot).name) + " (" + words + ")");
        return true;
    }

    int Processor::forgetAllMidi()
    {
        const int n = learn_.forgetAll();
        if (n > 0)
            journal_.note(Journal::Who::user, "forgot every MIDI control learned (" + std::to_string(n) + ")");
        return n;
    }

    float Processor::dialDefault01(int c) const
    {
        return isPanelDial(c) || c < 0 || c >= kHostControls ? 0.5f : params_[static_cast<std::size_t>(c)]->getDefaultValue();
    }

    bool Processor::openVersion(const projectstore::Project& p, const projectstore::Version& v, std::string& why,
                                ChatAgent::OwnerChange how)
    {
        Draft d;
        d.files = tidyFiles(v.files);
        if (d.files.empty())
        {
            why = "\"" + p.name + "\" version " + std::to_string(v.number) + " has no program";
            return false;
        }
        d.active = std::clamp(v.active, 0, static_cast<int>(d.files.size()) - 1);
        std::vector<midilearn::Mapping> left = learn_.mappings();
        // Its samples (none: none), decoded before it compiles with them (ProcessorAssets.cpp).
        std::vector<std::string> refused;
        const Assets samples = assetsOfVersion(p, v, assetsNow(), refused);
        replaceAssets(samples, Journal::Who::user, "");
        if (!refused.empty())
            journal_.note(Journal::Who::user, "left out of the version's samples: " + refused.front()
                                                  + (refused.size() > 1 ? " (and " + std::to_string(refused.size() - 1) + " more)" : ""));
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            draft_ = d;
            ++draftRevision_;                                    // the editor shows the version's files
            projectAssets_ = samples;
            projectName_ = p.name;
            projectVersion_ = v.number;
            projectLatest_ = p.latest().number;
            projectPreset_.clear();
            projectFiles_ = d.files;
            baseDials_ = v.dials;
            if (v.midi.empty())
                midiOpened_ = std::move(left);                   // what it leaves: no change of its own (saveVersion)
            else
                midiOpened_.reset();
        }
        projectDials_ = v.dials;
        for (const projectstore::Answer& a : v.circuits)            // its netlists' answers, before it compiles
            circuits::restore(a.key, a.text);
        if (!v.midi.empty())                                     // its MIDI learn's mappings, when it has some (a
            learn_.setMappings(v.midi);                          // version with none leaves those there are)
        // The version as it was saved: compiled (its netlists' answers in, its program from its init), never a panel's
        // edit (Compiler.h).
        const uint64_t id = compiler_.request(d.files, false);
        dialsRequest_ = id;
        syncChatOwner(how, &p);                                  // another project: its chats (or the chat goes along)
        return true;
    }

    bool Processor::openProject(const std::string& name, int version, std::string& why)
    {
        return openProjectAs(name, version, Journal::Who::user, why);
    }

    bool Processor::openProjectAs(const std::string& name, int version, std::optional<Journal::Who> who, std::string& why)
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
        // The plugin's own chat asked for it in a turn: the conversation that asked goes on with the project (carry, as
        // a save), never left behind with the project before (CLAUDE.md, "The chats"). Claude Desktop's open is the
        // user's own chat's change of project, as PROJECTS' (open: at the call's end, ChatAgent's pendingOwner_).
        if (!openVersion(*p, *v, why, who == Journal::Who::chat ? ChatAgent::OwnerChange::carry : ChatAgent::OwnerChange::open))
            return false;
        if (who)
            journal_.dialsNote(*who, "opened project \"" + p->name + "\", version " + std::to_string(v->number));
        refreshPresets(true);
        return true;
    }

    // A state load (not a host's program call): the session's project read whole, outside programMutex_. Its base is
    // as openVersion and applyPreset leave it: the version's dials, the preset applied over them. A project gone, or a
    // version no more kept, leaves what is known (none: nothing reads as edited). The read also puts its presets in the
    // store's cache, so refreshPresets has them at once, even in a host just started.
    void Processor::restoreProjectBase()
    {
        std::string name, preset;
        int version = 0;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            name = projectName_;
            preset = projectPreset_;
            version = projectVersion_;
        }
        if (name.empty())
            return;
        const std::optional<projectstore::Project> p = projectstore::load(name);
        if (!p)
            return;
        std::vector<projectstore::Dial> base;
        if (const projectstore::Version* v = p->version(version))
            base = v->dials;
        if (const projectstore::Preset* pr = preset.empty() ? nullptr : p->preset(preset))
            base = overlay(std::move(base), pr->dials);
        const std::lock_guard<std::mutex> lock(programMutex_);
        if (projectName_ == name && projectPreset_ == preset && projectVersion_ == version)
            baseDials_ = std::move(base);
    }

    // EXAMPLES and RANDOM (EditorPort::startAfresh): `files` as a new instance plays Init. They are the whole draft, no
    // project is open, and the dials go to their defaults when they have compiled. A version's dials still on their
    // way are dropped.
    void Processor::startAfresh(const Sources& files, const std::string& what)
    {
        Draft d;
        d.files = tidyFiles(files);
        if (d.files.empty())
            return;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            draft_ = d;
            ++draftRevision_;                                    // the editor shows the new files
            projectName_.clear();
            projectVersion_ = projectLatest_ = 0;
            projectPreset_.clear();
            projectFiles_ = d.files;                             // nothing to lose yet, as in a new instance
            projectAssets_ = assetsNow();                        // the samples stay: the user's, for the next program
            baseDials_.clear();
            midiOpened_.reset();
        }
        projectDials_.clear();
        dialsRequest_ = 0;
        setGuiPages({});
        syncChatOwner(ChatAgent::OwnerChange::open);             // no project: this instance's chats
        chat_.clearChecks();
        const uint64_t id = compiler_.request(d.files, false);   // a fresh start: compiled, whatever changed
        defaultsRequest_ = id;
        userRun_.store(id);
        journal_.dialsNote(Journal::Who::user,
                           "started afresh from " + what + " (its files only, every dial at its default)");
        refreshPresets(true);
    }

    bool Processor::saveVersion(const std::string& name, SavedVersion& saved, std::string& why)
    {
        return saveVersionAs(name, {}, Journal::Who::user, saved, why);
    }

    bool Processor::saveVersionAs(const std::string& name, const std::string& lead, std::optional<Journal::Who> who,
                                  SavedVersion& saved, std::string& why)
    {
        Sources files;
        int active = 0;
        std::string open;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            files = draft_.files;
            active = draft_.active;
            open = projectName_;
        }
        const std::vector<projectstore::Dial> dials = dialsNow();
        // The self-test's answers for its netlists (fminus/Circuits.h), kept with the version.
        std::vector<projectstore::Answer> answers;
        for (auto& [key, text] : circuits::answers().of(circuits::keysOf(files)))
            answers.push_back({ std::move(key), std::move(text) });
        // MIDI learn's mappings; those a version with none left as they were (opening it) are no change of their own.
        projectstore::VersionMidi midi{ learn_.mappings() };
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            midi.inherited = midiOpened_ && *midiOpened_ == midi.mappings
                          && projectstore::sameName(projectstore::tidyName(name), projectName_);
        }
        const Assets samples = assetsNow();
        // A factory project is never written: its name, or a new project's while one is open, saves the user's own copy.
        if (isFactoryName(name) || (isFactoryName(open) && !projectstore::exists(name)))
            return saveCopy(isFactoryName(name) ? name : open, isFactoryName(name) ? std::string() : name, files, active, dials,
                            answers, midi.mappings, samples, lead, who, saved, why);
        const std::optional<projectstore::Saved> r =
            projectstore::saveVersion(name, files, active, dials, answers, midi, samples.get(), &why, lead);
        if (!r)
            return false;
        saved = { r->number, r->unchanged, r->created, projectstore::tidyName(name), false };
        if (!r->unchanged && who)
            journal_.note(*who, "saved version " + std::to_string(r->number) + " of \"" + projectstore::tidyName(name) + "\"");
        std::unique_lock<std::mutex> lock(programMutex_);
        const std::string tidy = projectstore::tidyName(name);
        // The preset applied stays named while the dials are still its.
        bool keep = projectstore::sameName(tidy, projectName_) && !projectPreset_.empty();
        for (const projectstore::Dial& d : dials)
            for (const projectstore::Dial& b : baseDials_)
                if (b.name == d.name && b.value != d.value)
                    keep = false;
        if (!keep)
            projectPreset_.clear();
        // The project's id as the save left its file: another than the one kept (a new file made where the project
        // open's was deleted or renamed away since it was opened) is looked up again (chatOwnerNow).
        if (r->id.empty() || r->id != projectId_ || !projectstore::sameName(tidy, projectIdName_))
            projectIdName_.clear();
        projectName_ = tidy;
        projectVersion_ = r->number;
        projectLatest_ = r->number;
        projectFiles_ = files;
        projectAssets_ = samples;
        baseDials_ = dials;
        presetsStale_.store(true);                               // the next poll reads its presets (refreshPresets)
        if (!r->unchanged)
            midiOpened_.reset();                                 // the version saved has these mappings
        lock.unlock();
        syncChatOwner(ChatAgent::OwnerChange::carry);            // the conversation goes on with what it made
        return true;
    }

    // The user's own copy of the factory project `from` (Factory.h): a new project named `as` (else the factory one's
    // own name), or "<name> 2", ... when one has it (projectstore::add), of one version, the editor's files and dials,
    // its note saying whose copy it is, with the factory project's presets and info. It is then the project open.
    bool Processor::saveCopy(const std::string& from, const std::string& as, const Sources& files, int active,
                             const std::vector<projectstore::Dial>& dials, const std::vector<projectstore::Answer>& answers,
                             const std::vector<midilearn::Mapping>& midi, const Assets& samples, const std::string& lead,
                             std::optional<Journal::Who> who, SavedVersion& saved, std::string& why)
    {
        if (files.empty())
        {
            why = "there is no program to save";
            return false;
        }
        const std::optional<projectstore::Project> origin = projectstore::load(from, &why);
        if (!origin)
            return false;
        projectstore::Project p;
        p.name = as.empty() ? ownName(origin->name) : projectstore::tidyName(as);
        if (p.name.empty())
        {
            why = "a project needs a name";
            return false;
        }
        projectstore::Version v;
        v.number = 1;
        v.savedMs = juce::Time::currentTimeMillis();
        v.midi = midilearn::tidy(midi);
        v.samples = projectstore::refsOf(samples.get());
        const std::string changed = projectstore::describeChange(&origin->latest(), files, dials, &v.midi, &v.samples);
        v.note = "a copy of the factory's \"" + ownName(origin->name) + "\"" + (changed == "the tab shown" ? "" : ", " + changed);
        if (const std::string l = projectstore::noteLead(lead); !l.empty())
            v.note = l + " (" + v.note + ")";
        v.files = files;
        v.active = active;
        v.dials = dials;
        v.circuits = answers;
        if (v.circuits.size() > projectstore::kMaxAnswers)
            v.circuits.resize(projectstore::kMaxAnswers);
        p.versions.push_back(std::move(v));
        p.presets = origin->presets;
        p.info = origin->info;
        if (samples != nullptr)                          // the copy's own sample files, as saveVersion keeps them
            for (const std::shared_ptr<const assets::Asset>& a : samples->items)
                if (std::none_of(p.samples.begin(), p.samples.end(), [&](const projectstore::SampleFile& f) { return f.hash == a->hash(); }))
                    p.samples.push_back({ a->hash(), a->bytes() });
        if (const std::string refused = projectstore::samplesRefusal(p); !refused.empty())
        {
            why = refused;
            return false;
        }
        const std::optional<std::string> added = projectstore::add(std::move(p), &why);
        if (!added)
            return false;
        saved = { 1, false, true, *added, true };
        if (who)
            journal_.note(*who, "saved \"" + *added + "\", a copy of the factory project \"" + ownName(origin->name)
                                    + "\" (its presets with it)");
        std::unique_lock<std::mutex> lock(programMutex_);
        projectName_ = *added;
        projectIdName_.clear();                                  // its new id is looked up (chatOwnerNow)
        projectVersion_ = projectLatest_ = 1;
        projectFiles_ = files;
        projectAssets_ = samples;
        midiOpened_.reset();                                     // the copy saved has these mappings
        // The preset applied stays named while the dials are still its: the copy has it too.
        for (const projectstore::Dial& d : dials)
            for (const projectstore::Dial& b : baseDials_)
                if (b.name == d.name && b.value != d.value)
                    projectPreset_.clear();
        baseDials_ = dials;
        presetsStale_.store(true);
        lock.unlock();
        syncChatOwner(ChatAgent::OwnerChange::carry);            // the conversation goes on with the copy it made
        return true;
    }

    // A project deleted, its chats on this Mac with it (the Projects card says so); while it was open, this instance's
    // chats show after it.
    bool Processor::deleteProject(const std::string& name, std::string& why)
    {
        const std::optional<projectstore::Project> p = projectstore::load(name);
        const std::string id = p ? p->id : std::string();
        if (!projectstore::remove(name, &why))
            return false;
        bool open = false;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            open = projectstore::sameName(projectName_, name);
            if (open)
            {
                projectName_.clear();
                projectVersion_ = projectLatest_ = 0;
                projectPreset_.clear();
                baseDials_.clear();
            }
        }
        presetsStale_.store(true);
        if (open)
            syncChatOwner(ChatAgent::OwnerChange::open);
        // Its chats, unless another project's file still holds its id (a copy made in the Finder).
        if (projectstore::validProjectId(id) && projectstore::nameOfId(id).empty())
            chatstore::removeOwner(id);
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
        return savePresetAs(preset, Journal::Who::user, why);
    }

    bool Processor::savePresetAs(const std::string& preset, std::optional<Journal::Who> who, std::string& why)
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
        if (isFactoryName(project))
        {
            why = "\"" + ownName(project) + "\" is a factory project, whose presets stay as they are: save the project first "
                  "(your own copy, its presets with it), then the preset";
            return false;
        }
        if (!projectstore::exists(project))
        {
            why = "\"" + project + "\" is not on " + kThisMachine + " any more: save it again first";
            return false;
        }
        const std::vector<projectstore::Dial> dials = dialsNow();
        if (!projectstore::savePreset(project, preset, dials, version, &why))
            return false;
        if (who)
            journal_.note(*who, "saved preset \"" + projectstore::tidyName(preset) + "\"");
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            projectPreset_ = projectstore::tidyName(preset);
            baseDials_ = dials;
        }
        refreshPresets(true);
        return true;
    }

    bool Processor::applyPreset(const std::string& project, const std::string& preset, std::string& why)
    {
        return applyPresetAs(project, preset, Journal::Who::user, why);
    }

    bool Processor::applyPresetAs(const std::string& project, const std::string& preset, std::optional<Journal::Who> who,
                                  std::string& why)
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
        std::array<std::vector<std::string>, kMaxControls> options;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            open = projectstore::sameName(projectName_, p->name);
            cs = controls_;
            options = panel_.options;
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
            setDialsByName(cs, options, pr->dials);
        syncOwnDials();                                          // the preset's dials: its line says so
        if (who)
            journal_.dialsNote(*who, "applied preset \"" + pr->name + "\" of \"" + p->name + "\"");
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            projectPreset_ = pr->name;
            baseDials_ = open ? overlay(baseDials_, pr->dials) : overlay(p->latest().dials, pr->dials);
        }
        refreshPresets(true);
        return true;
    }

    bool Processor::setPresetCategory(const std::string& project, const std::string& preset, const std::string& category,
                                      std::string& why)
    {
        if (!projectstore::setPresetCategory(project, preset, category, &why))
            return false;
        refreshPresets(true);
        return true;
    }

    // ---- the presets in turn (EditorPort::presetCycle; the host's programs; MIDI's program change) -------------------
    // The preset applied (by name, in `list`), else the first whose dials (at least one the program has) are all where
    // the dials are; -1: none. Any thread: the dials' values are atomics, the rest under programMutex_.
    int Processor::currentPreset(const std::vector<projectstore::PresetSummary>& list, bool& applied) const
    {
        applied = false;
        Controls cs;
        std::array<std::vector<std::string>, kMaxControls> options;
        std::string preset;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            preset = projectPreset_;
            if (preset.empty())
            {
                cs = controls_;
                options = panel_.options;
            }
        }
        if (!preset.empty())
        {
            for (std::size_t i = 0; i < list.size(); ++i)
                if (projectstore::sameName(list[i].name, preset))
                {
                    applied = true;
                    return static_cast<int>(i);
                }
            return -1;                                           // deleted since: none, not another that matches
        }
        for (std::size_t i = 0; i < list.size(); ++i)
            if (const auto [found, all] = dialsAgainst(cs, options, list[i].dials); found > 0 && all)
                return static_cast<int>(i);
        return -1;
    }

    // The project open's presets from the store's cache (never a file read whole: one it lacks goes to the Reader, and
    // the next read has it), when `force` or after kPresetsPollMs; and the program the host is told.
    void Processor::refreshPresets(bool force)
    {
        std::string project;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            project = projectName_;
        }
        const double now = juce::Time::getMillisecondCounterHiRes();
        bool read = false;
        {
            const std::lock_guard<std::mutex> lock(presetsMutex_);
            read = force || presetsStale_.exchange(false) || heldProject_ != project || now - heldAt_ >= kPresetsPollMs
                || reader_->revision() != heldRead_;
            if (read)
            {
                heldAt_ = now;
                heldRead_ = reader_->revision();
            }
        }
        if (read)
        {
            std::vector<projectstore::PresetSummary> list;
            if (!project.empty())
            {
                std::vector<std::string> unread;
                if (auto ps = projectstore::presetsCached(project, unread))
                    list = std::move(*ps);
                if (!unread.empty())
                    reader_->request(unread);
            }
            uint64_t names = 1469598103934665603ull;             // FNV-1a over the project's and the presets' names
            const auto mix = [&names](const std::string& t) {
                for (const char ch : t)
                    names = (names ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
                names = (names ^ 0xffu) * 1099511628211ull;
            };
            mix(project);
            for (const projectstore::PresetSummary& pr : list)
                mix(pr.name);
            const std::lock_guard<std::mutex> lock(presetsMutex_);
            heldProject_ = project;
            heldPresets_ = std::move(list);
            heldNames_ = names;
        }
        std::vector<projectstore::PresetSummary> list;
        std::string held;
        {
            const std::lock_guard<std::mutex> lock(presetsMutex_);
            list = heldPresets_;
            held = heldProject_;
        }
        bool applied = false;
        const int at = currentPreset(list, applied);
        const int count = programCount();
        // None applied or matched: the "-" past the presets in the VST3 (its 128th at most), the first elsewhere.
        const int none = wrapperType == wrapperType_VST3 ? std::min(static_cast<int>(list.size()), count - 1) : 0;
        const std::lock_guard<std::mutex> lock(presetsMutex_);
        if (at >= 0 && at < count)
        {
            hostCurrent_.store(at);
            hostFor_ = held;
        }
        else if (list.empty() || hostFor_ != held)
        {
            // Another project's number names another preset here: a VST3 host told it would take it for the one
            // playing and never apply that preset when chosen (JUCE sets a program only when it is not the current).
            hostCurrent_.store(none);
            hostFor_ = list.empty() ? std::string() : held;
        }
        // else kept: the dials match none, in the project it was set in
    }

    PresetCycle Processor::presetCycle() const
    {
        PresetCycle out;
        std::vector<projectstore::PresetSummary> list;
        {
            const std::lock_guard<std::mutex> lock(presetsMutex_);
            out.project = heldProject_;
            list = heldPresets_;
        }
        if (out.project.empty())
            return out;
        for (const projectstore::PresetSummary& pr : list)
            out.presets.push_back({ pr.name, {}, pr.from, pr.category });
        out.current = currentPreset(list, out.applied);
        if (out.applied)
        {
            Controls cs;
            std::array<std::vector<std::string>, kMaxControls> options;
            std::vector<projectstore::Dial> base;
            {
                const std::lock_guard<std::mutex> lock(programMutex_);
                cs = controls_;
                options = panel_.options;
                base = baseDials_;
            }
            out.edited = dialsRequest_ == 0 && !dialsAgainst(cs, options, base).second;
        }
        return out;
    }

    bool Processor::stepPreset(int step, std::string& why)
    {
        refreshPresets(false);
        const PresetCycle c = presetCycle();
        if (c.project.empty())
        {
            why = "open a project first: the arrows step through its presets";
            return false;
        }
        const int n = static_cast<int>(c.presets.size());
        if (n == 0)
        {
            why = "\"" + ownName(c.project) + "\" has no presets yet";
            return false;
        }
        const int at = c.current < 0 ? (step >= 0 ? 0 : n - 1) : ((c.current + step) % n + n) % n;
        return applyPreset(c.project, c.presets[static_cast<std::size_t>(at)].name, why);
    }

    // A program by number (the host's list, MIDI's program change): the project open's preset at that index.
    bool Processor::choosePreset(int index, const char* who)
    {
        refreshPresets(false);
        std::string project, name;
        {
            const std::lock_guard<std::mutex> lock(presetsMutex_);
            if (index < 0 || index >= static_cast<int>(heldPresets_.size()) || heldProject_.empty())
                return false;
            project = heldProject_;
            name = heldPresets_[static_cast<std::size_t>(index)].name;
        }
        std::string why;
        if (!applyPresetAs(project, name, Journal::Who::host, why))
        {
            journal_.note(Journal::Who::host, std::string(who) + " chose preset " + std::to_string(index + 1) + ", not applied: " + why);
            return false;
        }
        return true;
    }

    // The host's list: the project open's presets. A VST3 host reads how many when the plugin is made and never again
    // (JUCE's program parameter), so it is told kVst3Programs, those past the presets named "-".
    int Processor::programCount() const
    {
        if (wrapperType == wrapperType_VST3)
            return kVst3Programs;
        const std::lock_guard<std::mutex> lock(presetsMutex_);
        return std::max(1, static_cast<int>(heldPresets_.size()));
    }

    bool Processor::deletePreset(const std::string& project, const std::string& preset, std::string& why)
    {
        if (!projectstore::removePreset(project, preset, &why))
            return false;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            if (projectstore::sameName(projectName_, project) && projectstore::sameName(projectPreset_, preset))
                projectPreset_.clear();
        }
        refreshPresets(true);
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

    bool Processor::setProjectCategory(const std::string& project, const std::string& category, std::string& why)
    {
        return projectstore::setCategory(project, category, &why);
    }

    ProjectsView Processor::projectsView() const
    {
        const projectstore::View v = projectstore::view();
        return { v.favourites, v.folded, v.search, v.tag, v.byDate };
    }

    bool Processor::setProjectsView(const ProjectsView& view, std::string& why)
    {
        return projectstore::setView({ view.favourites, view.folded, view.search, view.tag, view.byDate }, &why);
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
        projectstore::Project q = *p;
        q.name = ownName(q.name);                                // a factory project's: its own name
        if (!(package ? projectexport::writePackage(q, allVersions, to, r, &why) : projectexport::writeSource(q, allVersions, to, r, &why)))
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

    bool Processor::chatSetDraftIf(const Draft& draft, const Sources& read)
    {
        if (read.empty())
            return false;
        for (const SourceFile& r : read)
            if (findFile(draft.files, r.name) == nullptr)
                return false;
        Draft before, d;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            for (const SourceFile& r : read)
                if (const SourceFile* now = findFile(draft_.files, r.name); now == nullptr || now->text != r.text)
                    return false;                        // a gesture or typing synced meanwhile: never overwritten
            // Only these files' texts are written (and the tab shown), all in one change: the others stay as they are
            // now, whatever the user typed in them since the tool read the draft.
            d = draft_;
            for (SourceFile& f : d.files)
                if (findFile(read, f.name) != nullptr)
                    f.text = findFile(draft.files, f.name)->text;
            const auto shown = draft.active >= 0 && draft.active < static_cast<int>(draft.files.size())
                                   ? std::find_if(d.files.begin(), d.files.end(), [&](const SourceFile& f) {
                                         return f.name == draft.files[static_cast<std::size_t>(draft.active)].name;
                                     })
                                   : d.files.end();
            if (shown != d.files.end())
                d.active = static_cast<int>(shown - d.files.begin());
            before = std::exchange(draft_, d);
            ++draftRevision_;                            // the editor merges it into its tabs
        }
        if (!chat_.mcpWorking())
            journalDraft(before, d, Journal::Who::chat);
        return true;
    }

    std::string Processor::chatSchematicPins(const std::string& file) const { return schematicPins(file); }

    CompileStatus Processor::chatRun(const Sources& files)
    {
        const uint64_t id = compiler_.request(files, panelEdits());
        compiler_.wait();
        const CompileStatus s = compiler_.status();
        if (!chat_.mcpWorking() && s.request >= id)
        {
            if (s.stopped)
                chatStopJournaled_.store(s.stoppedProgram);     // said with the run: poll() says it no more
            if (s.ok && !s.panelOnly)
                journal_.dialsNote(Journal::Who::chat, runWords(s));
            else
                journal_.note(Journal::Who::chat, runWords(s));
        }
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

    Sources Processor::chatPlaying(Assets& samples) const
    {
        const CompileStatus s = compiler_.status();
        if (s.ok)
        {
            samples = s.assets;
            return s.sources;                                    // as chatRunning
        }
        const std::lock_guard<std::mutex> lock(programMutex_);
        samples = runningAssets_;
        return running_;
    }

    bool Processor::changesProgram(const CompileStatus& s) const
    {
        return !running_.empty() && (!cir::sameProgram(running_, s.sources) || runningSamples_ != s.samples);
    }

    Sources Processor::chatPrevious(Assets& samples) const
    {
        const CompileStatus s = compiler_.status();
        const std::lock_guard<std::mutex> lock(programMutex_);
        // poll() has not taken the newest yet: it comes after this (a panel's edit of a program it has not: its
        // compile).
        if (s.ok && !(s.panelOnly && s.program == program_) && changesProgram(s))
        {
            samples = runningAssets_;
            return running_;
        }
        samples = previousAssets_;
        return previous_;
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
        const auto i = static_cast<std::size_t>(slot);
        // One status for the control and its carry (chatDials' rule: a failed compile's are the program's that plays).
        const CompileStatus s = compiler_.status();
        Control c;
        if (s.ok)
            c = s.controls[i];
        else
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            c = controls_[i];
        }
        {
            // A value Claude set that poll() has not applied yet is the dial's now: a measurement right after set_dial
            // renders it (as the parameter will hold it: through its range and steps).
            const std::lock_guard<std::mutex> lock(dialMutex_);
            for (auto it = pendingDials_.rbegin(); it != pendingDials_.rend(); ++it)
                if (it->first == slot)
                    return plainOf(c, normOf(c, it->second));
        }
        // Whose controls the value is in, then the value, as readParameters reads them (apply() stores the meaning after
        // its writes): under the old meaning the value is the one the carry was made from or the one apply() wrote, and
        // under the new one it is the written one.
        const uint64_t meaning = meaningProgram_.load(std::memory_order_acquire);
        const float norm = raw_[i]->load();
        if (!std::isfinite(norm))
            return c.def;
        // A program compiled whose carry poll() has not written yet: the value it plays (Engine.h, "The dials' carry").
        if (s.ok && meaning != s.program && meaning == s.carry.from && (s.carry.slots & slotBit(slot)) != 0
            && std::clamp(norm, 0.0f, 1.0f) == s.carry.fromNorm[i])
            return s.carry.plain[i];
        return plainOf(c, norm);
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
            o.freshFrom = compiler_.request(running, false);
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
