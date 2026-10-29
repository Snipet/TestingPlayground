// Source/plugin/standalone/Devices.cpp: see Devices.h.
#include "plugin/standalone/Devices.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playground::standalone
{
    namespace
    {
        // JUCE's AudioDeviceSelectorComponent's name for a pair ("Output 1" and "Output 2": "Output 1 + 2"): the second's
        // name after what the two share, split only at a space ("input 11" + "input 12" never "input 11 + 2").
        juce::String pairName(const juce::String& name1, const juce::String& name2)
        {
            juce::String common;
            for (int j = 0; j < name1.length(); ++j)
                if (name1.substring(0, j).equalsIgnoreCase(name2.substring(0, j)))
                    common = name1.substring(0, j);
            while (common.isNotEmpty() && !juce::CharacterFunctions::isWhitespace(common.getLastCharacter()))
                common = common.dropLastCharacters(1);
            return name1.trim() + " + " + name2.substring(common.length()).trim();
        }

        // A device's channels in pairs as the selector shows them (a last odd one alone), each on when either of its
        // two is.
        std::vector<ChannelPair> pairsOf(const juce::StringArray& names, const juce::BigInteger& on)
        {
            std::vector<ChannelPair> out;
            for (int i = 0; i < names.size(); i += 2)
            {
                ChannelPair p;
                p.name = (i + 1 >= names.size() ? names[i].trim() : pairName(names[i], names[i + 1])).toStdString();
                p.on = on[i] || on[i + 1];
                out.push_back(std::move(p));
            }
            return out;
        }

        // The selector's flipBit: a channel (here a pair) off, while more than `least` are on; or on, turning off the
        // first one on (or the highest) when `most` are on already.
        void flipBit(juce::BigInteger& chans, int index, int least, int most)
        {
            const int active = chans.countNumberOfSetBits();
            if (chans[index])
            {
                if (active > least)
                    chans.setBit(index, false);
            }
            else
            {
                if (active >= most)
                {
                    const int first = chans.findNextSetBit(0);
                    chans.clearBit(index > first ? first : chans.getHighestBit());
                }
                chans.setBit(index, true);
            }
        }

        double msOf(int samples, double rate)
        {
            return rate > 0.0 ? static_cast<double>(samples) * 1000.0 / rate : 0.0;
        }
    }

    Devices::Devices(juce::StandalonePluginHolder& holder) : holder_(holder)
    {
        holder_.deviceManager.addChangeListener(this);
        listenToTypes();
        muted_.referTo(holder_.getMuteInputValue());
        muted_.addListener(this);
        midiList_ = juce::MidiDeviceListConnection::make([this] { changed(); });
    }

    Devices::~Devices()
    {
        midiList_.reset();
        muted_.removeListener(this);
        for (juce::AudioIODeviceType* t : types_)
            t->removeListener(this);
        holder_.deviceManager.removeChangeListener(this);
    }

    void Devices::listenToTypes()
    {
        for (juce::AudioIODeviceType* t : holder_.deviceManager.getAvailableDeviceTypes())
            if (std::find(types_.begin(), types_.end(), t) == types_.end())
            {
                t->addListener(this);
                types_.push_back(t);
            }
    }

    int Devices::maxChannels(bool input) const
    {
        int most = 0;
        if (holder_.channelConfiguration.size() > 0)
        {
            const auto& config = holder_.channelConfiguration.getReference(0);
            most = std::max(0, static_cast<int>(input ? config.numIns : config.numOuts));
        }
        if (holder_.processor != nullptr)
            if (auto* bus = holder_.processor->getBus(input, 0))
                most = std::max(0, bus->getDefaultLayout().size());
        return most;
    }

    DeviceSetup Devices::setup() const
    {
        juce::AudioDeviceManager& dm = holder_.deviceManager;
        DeviceSetup s;
        for (juce::AudioIODeviceType* t : dm.getAvailableDeviceTypes())
            s.types.push_back(t->getTypeName().toStdString());
        s.type = dm.getCurrentAudioDeviceType().toStdString();
        const juce::AudioDeviceManager::AudioDeviceSetup config = dm.getAudioDeviceSetup();
        if (juce::AudioIODeviceType* type = dm.getCurrentDeviceTypeObject())
        {
            s.separateInputs = type->hasSeparateInputsAndOutputs();
            for (const juce::String& n : type->getDeviceNames(false))
                s.outputs.push_back(n.toStdString());
            for (const juce::String& n : type->getDeviceNames(true))
                s.inputs.push_back(n.toStdString());
        }
        s.output = config.outputDeviceName.toStdString();
        s.input = config.inputDeviceName.toStdString();
        s.hasInput = maxChannels(true) > 0;
        if (juce::AudioIODevice* device = dm.getCurrentAudioDevice())
        {
            s.open = true;
            if (maxChannels(false) > 0)
                s.outputPairs = pairsOf(device->getOutputChannelNames(), config.outputChannels);
            if (s.hasInput)
                s.inputPairs = pairsOf(device->getInputChannelNames(), config.inputChannels);
            for (const double r : device->getAvailableSampleRates())
                s.rates.push_back(r);
            s.rate = device->getCurrentSampleRate();
            const double rate = s.rate > 0.0 ? s.rate : 48000.0;   // the selector's when the device says none
            for (const int b : device->getAvailableBufferSizes())
                s.buffers.push_back({ b, msOf(b, rate) });
            s.buffer = device->getCurrentBufferSizeSamples();
            s.bufferMs = msOf(s.buffer, rate);
            s.inputLatency = s.hasInput ? device->getInputLatencyInSamples() : 0;
            s.outputLatency = device->getOutputLatencyInSamples();
            s.inputLatencyMs = msOf(s.inputLatency, rate);
            s.outputLatencyMs = msOf(s.outputLatency, rate);
            if (error_.empty())
                if (const juce::String e = device->getLastError(); e.isNotEmpty())
                {
                    s.error = e.toStdString();
                    s.errorOf = DeviceSection::output;
                }
        }
        s.takesMidi = holder_.processor != nullptr && holder_.processor->acceptsMidi();
        if (s.takesMidi)
            for (const juce::MidiDeviceInfo& m : juce::MidiInput::getAvailableDevices())
                s.midi.push_back({ m.name.toStdString(), m.identifier.toStdString(), dm.isMidiInputDeviceEnabled(m.identifier) });
        s.muteApplies = holder_.getProcessorHasPotentialFeedbackLoop();
        s.muted = static_cast<bool>(holder_.getMuteInputValue().getValue());
        if (!error_.empty())
        {
            s.error = error_;
            s.errorOf = errorOf_;
        }
        s.serial = serial();
        return s;
    }

    void Devices::apply(const juce::AudioDeviceManager::AudioDeviceSetup& config, DeviceSection section)
    {
        const juce::String error = holder_.deviceManager.setAudioDeviceSetup(config, true);
        error_ = error.toStdString();
        errorOf_ = section;
        save();
        changed();
    }

    void Devices::chooseType(const std::string& type)
    {
        juce::AudioDeviceManager& dm = holder_.deviceManager;
        dm.setCurrentAudioDeviceType(juce::String(type), true);
        listenToTypes();
        if (juce::AudioIODeviceType* t = dm.getCurrentDeviceTypeObject())
            t->scanForDevices();                         // as the selector does for the type it shows
        error_.clear();                                  // the type's device says its own (getLastError), or none opens
        save();
        changed();
    }

    // As the selector's updateConfig: the device's name (both, when the type has no separate inputs), its default
    // channels.
    void Devices::chooseOutput(const std::string& device)
    {
        auto config = holder_.deviceManager.getAudioDeviceSetup();
        config.outputDeviceName = juce::String(device);
        if (juce::AudioIODeviceType* t = holder_.deviceManager.getCurrentDeviceTypeObject(); t != nullptr && !t->hasSeparateInputsAndOutputs())
            config.inputDeviceName = config.outputDeviceName;
        config.useDefaultOutputChannels = true;
        apply(config, DeviceSection::output);
    }

    void Devices::chooseInput(const std::string& device)
    {
        auto config = holder_.deviceManager.getAudioDeviceSetup();
        config.inputDeviceName = juce::String(device);
        if (juce::AudioIODeviceType* t = holder_.deviceManager.getCurrentDeviceTypeObject(); t != nullptr && !t->hasSeparateInputsAndOutputs())
            config.outputDeviceName = config.inputDeviceName;
        config.useDefaultInputChannels = true;
        apply(config, DeviceSection::input);
    }

    void Devices::chooseRate(double rate)
    {
        auto config = holder_.deviceManager.getAudioDeviceSetup();
        config.sampleRate = static_cast<double>(std::lround(rate));   // the selector's ids are whole Hz
        apply(config, DeviceSection::audio);
    }

    void Devices::chooseBuffer(int samples)
    {
        auto config = holder_.deviceManager.getAudioDeviceSetup();
        config.bufferSize = samples;
        apply(config, DeviceSection::audio);
    }

    // The selector's ChannelSelectorListBox::flipEnablement, in stereo pairs, within the dialog's limits (0 to the
    // product's channels).
    void Devices::switchPair(bool input, int pair, bool on)
    {
        auto config = holder_.deviceManager.getAudioDeviceSetup();
        juce::BigInteger& original = input ? config.inputChannels : config.outputChannels;
        juce::BigInteger bits;
        for (int i = 0; i < 256; i += 2)
            bits.setBit(i / 2, original[i] || original[i + 1]);
        if (pair < 0 || pair >= 128 || bits[pair] == on)
            return;
        if (input)
            config.useDefaultInputChannels = false;
        else
            config.useDefaultOutputChannels = false;
        flipBit(bits, pair, 0, maxChannels(input) / 2);
        for (int i = 0; i < 256; ++i)
            original.setBit(i, bits[i / 2]);
        apply(config, input ? DeviceSection::input : DeviceSection::output);
    }

    void Devices::switchMidi(const std::string& identifier, bool on)
    {
        holder_.deviceManager.setMidiInputDeviceEnabled(juce::String(identifier), on);
        save();
        changed();
    }

    void Devices::setMuted(bool muted)
    {
        holder_.getMuteInputValue().setValue(muted);    // the holder's own listener mutes the audio callback's input
        save();
        changed();
    }

    void Devices::playTestTone() { holder_.deviceManager.playTestSound(); }

    void Devices::save()
    {
        holder_.saveAudioDeviceState();                  // "audioSetup" and "shouldMuteInput", as at JUCE's shutdown
        if (auto* file = dynamic_cast<juce::PropertiesFile*>(holder_.settings.get()))
            file->saveIfNeeded();
    }

    void Devices::changeListenerCallback(juce::ChangeBroadcaster*)
    {
        listenToTypes();
        changed();
    }

    void Devices::audioDeviceListChanged() { changed(); }

    void Devices::valueChanged(juce::Value&)
    {
        save();                                          // a mute changed elsewhere persists at once too
        changed();
    }
}
