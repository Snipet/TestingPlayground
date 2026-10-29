// Source/plugin/DevicePort.h: the Standalone's audio and MIDI devices, as the editor's settings card shows and changes
// them (Source/editor/SettingsCard.h), in plain data (JUCE-free: the editor includes it through EditorPort.h).
//
// The Standalone implements it over JUCE's StandalonePluginHolder (its juce::AudioDeviceManager and the mute of its
// input: Source/plugin/standalone/Devices.h) and hands it to the editor with its window's hooks
// (Processor::WindowHooks, EditorPort::devices). In a host there is none: the AU and the VST3 have no card.
//
// Everything is on the message thread: setup() reads, the actions change, and serial() tells the card when to read
// again (a device plugged in or out, a choice made elsewhere, one of its own). The audio thread never sees any of it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace playground
{
    // Where a device's error belongs on the card: under the section whose choice failed.
    enum class DeviceSection : uint8_t { output, input, audio, midi };

    // A pair of channels as JUCE's device selector shows them ("Output 1 + 2"; a last one left alone, by its name), and
    // whether either of the two is on.
    struct ChannelPair
    {
        std::string name;
        bool on = false;
        friend bool operator==(const ChannelPair&, const ChannelPair&) = default;
    };

    struct MidiInputDevice
    {
        std::string name;
        std::string identifier;                  // what switchMidi takes
        bool on = false;
        friend bool operator==(const MidiInputDevice&, const MidiInputDevice&) = default;
    };

    struct BufferChoice
    {
        int samples = 0;
        double ms = 0.0;                         // at the current rate
        friend bool operator==(const BufferChoice&, const BufferChoice&) = default;
    };

    struct DeviceSetup
    {
        std::vector<std::string> types;          // the device types (CoreAudio, ...): the card shows them when several
        std::string type;                        // the current one
        bool separateInputs = true;              // its inputs are listed apart from its outputs (else one device is both)

        std::vector<std::string> outputs;        // the type's output devices
        std::string output;                      // the chosen one ("" for none)
        std::vector<ChannelPair> outputPairs;    // its channels, in pairs, and which are on

        bool hasInput = false;                   // the product takes audio in (an effect): the card's Input section
        std::vector<std::string> inputs;
        std::string input;                       // "" for none
        std::vector<ChannelPair> inputPairs;

        bool open = false;                       // a device is open (the rates, the buffers and the latency are its)
        std::vector<double> rates;               // Hz
        double rate = 0.0;
        std::vector<BufferChoice> buffers;
        int buffer = 0;                          // samples
        double bufferMs = 0.0;
        int inputLatency = 0, outputLatency = 0; // samples, as the device reports them (its buffer included)
        double inputLatencyMs = 0.0, outputLatencyMs = 0.0;

        bool takesMidi = false;                  // the product takes MIDI (an instrument): the card's MIDI section
        std::vector<MidiInputDevice> midi;       // the MIDI inputs, and which are on

        bool muteApplies = false;                // an effect's input may feed back through speakers: JUCE's guard
        bool muted = false;                      // the input is muted (silence in place of it)

        std::string error;                       // the device's last error ("" for none): a choice refused, a device
        DeviceSection errorOf = DeviceSection::output;   // that would not open; and the section it belongs under

        uint64_t serial = 0;                     // serial() when it was read
    };

    class DevicePort
    {
    public:
        virtual ~DevicePort() = default;

        virtual DeviceSetup setup() const = 0;
        // Changes whenever anything setup() returns may have: a device plugged in or out, a MIDI input coming or going, a
        // choice made here or elsewhere, the input muted or unmuted, an error.
        virtual uint64_t serial() const = 0;

        virtual void chooseType(const std::string& type) = 0;
        virtual void chooseOutput(const std::string& device) = 0;
        virtual void chooseInput(const std::string& device) = 0;     // "" for none
        virtual void chooseRate(double rate) = 0;
        virtual void chooseBuffer(int samples) = 0;
        // A channel pair on or off, as JUCE's selector switches one (a pair more than the product takes turns another
        // off; at least none).
        virtual void switchPair(bool input, int pair, bool on) = 0;
        virtual void switchMidi(const std::string& identifier, bool on) = 0;
        // Mute or unmute an effect's input. Unmuting is the user's choice only: nothing calls it for them.
        virtual void setMuted(bool muted) = 0;
        virtual void playTestTone() = 0;
    };
}
