// Source/plugin/standalone/Devices.h: the Standalone's audio and MIDI devices as the editor's settings card sees them
// (plugin/DevicePort.h), over juce::StandalonePluginHolder: its juce::AudioDeviceManager and the mute of its input.
// Compiled into the Standalone only, like App.cpp.
//
// It keeps what JUCE's own dialog (StandalonePluginHolder::showAudioSettingsDialog: an AudioDeviceSelectorComponent)
// did: the channel limits the holder passed it (the processor's main buses' default layouts: at most one stereo pair in
// and one out), the channels shown in pairs and named as JUCE names them, a choice applied with setAudioDeviceSetup as
// the user's (its error kept for the card), the device type's devices scanned when it opens, MIDI inputs from
// juce::MidiInput::getAvailableDevices and switched with setMidiInputDeviceEnabled (an instrument's only: the product
// takes MIDI), and playTestSound. The choices persist exactly as JUCE's did, in its settings file under its keys
// ("audioSetup", the manager's state XML, and "shouldMuteInput"): JUCE writes them only when the holder goes
// (saveAudioDeviceState, at shutdown); here they are written after every change too, and the file saved, so a crash
// keeps them.
//
// serial() moves with every change the card should see: the manager's change messages (a device opened, closed or set
// up, its list changed), each device type's list changing (hot plug), the MIDI inputs' list changing
// (juce::MidiDeviceListConnection), the mute changing, and the actions. Message thread only.
#pragma once

#include "plugin/DevicePort.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>   // after juce_audio_utils, which it needs

#include <atomic>
#include <string>
#include <vector>

namespace playground::standalone
{
    class Devices final : public DevicePort,
                          private juce::ChangeListener,
                          private juce::AudioIODeviceType::Listener,
                          private juce::Value::Listener
    {
    public:
        explicit Devices(juce::StandalonePluginHolder& holder);
        ~Devices() override;

        DeviceSetup setup() const override;
        uint64_t serial() const override { return serial_.load(std::memory_order_relaxed); }

        void chooseType(const std::string& type) override;
        void chooseOutput(const std::string& device) override;
        void chooseInput(const std::string& device) override;
        void chooseRate(double rate) override;
        void chooseBuffer(int samples) override;
        void switchPair(bool input, int pair, bool on) override;
        void switchMidi(const std::string& identifier, bool on) override;
        void setMuted(bool muted) override;
        void playTestTone() override;

    private:
        // The most channels the product takes in and out (as showAudioSettingsDialog computes them; the least is 0).
        int maxChannels(bool input) const;
        // The setup applied as the user's choice; its error kept for `section` (cleared when it opens).
        void apply(const juce::AudioDeviceManager::AudioDeviceSetup& config, DeviceSection section);
        void changed() noexcept { serial_.fetch_add(1, std::memory_order_relaxed); }
        void save();                                     // audioSetup and shouldMuteInput, written and saved

        void changeListenerCallback(juce::ChangeBroadcaster*) override;
        void audioDeviceListChanged() override;
        void valueChanged(juce::Value&) override;
        void listenToTypes();                            // each device type's list (they are made on first use)

        juce::StandalonePluginHolder& holder_;
        std::atomic<uint64_t> serial_{ 1 };
        std::string error_;
        DeviceSection errorOf_ = DeviceSection::output;
        juce::Value muted_;                              // refers to the holder's shouldMuteInput
        std::vector<juce::AudioIODeviceType*> types_;    // the types listened to
        juce::MidiDeviceListConnection midiList_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Devices)
    };
}
