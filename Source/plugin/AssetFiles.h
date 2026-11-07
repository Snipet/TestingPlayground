// Source/plugin/AssetFiles.h: the samples' files (dsp/Assets.h) read and written with JUCE: an audio file's bytes
// decoded by JUCE's readers for WAV, AIFF and FLAC (juce_audio_formats: nothing else, never Core Audio's own), the
// sound planar at the file's own rate, every channel's sample as the file holds it (a 32-bit float file's floats bit
// for bit; a 16-bit or 24-bit file's integers over 2^15 or 2^23, exactly); a file read from disk with its size checked
// before it is read; and the bytes as base64 for a session's XML and a project's JSON.
//
// Off the audio thread only: the message thread (a drop, the explorer's Add a sample...), Claude's worker (write_file's
// path), a state load. decodes() counts the decodes, so a probe can check that playing never decodes.
#pragma once

#include "dsp/Assets.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace playground::assetfiles
{
    // The sound of a file's bytes (its name in the words), or nothing, with why: a file JUCE's WAV, AIFF and FLAC readers
    // cannot read, a sound refusal() refuses (no frames, too many channels, a rate outside 1 to 768 kHz, too long, too
    // large decoded: what the header claims, before anything is allocated), or a WAV or AIFF whose header names more
    // sound than its bytes hold (damaged, or cut short).
    std::optional<assets::Audio> decode(const std::string& bytes, std::string_view name, std::string& why);
    // A sample of those bytes under `name`, checked against what `to` holds (refusal()'s caps) and decoded: null, with
    // why, when it cannot be one.
    std::shared_ptr<const assets::Asset> make(const assets::Set* to, const std::string& name, std::string bytes, std::string& why);
    // The file at `path` as a sample named `name` ("" for its own name, tidied: assets::tidyName; a sample of that name
    // in `to` is the one it replaces), its size checked before it is read: null, with why.
    std::shared_ptr<const assets::Asset> read(const assets::Set* to, const std::string& path, std::string name, std::string& why);
    uint64_t decodes() noexcept;

    std::string toBase64(const std::string& bytes);
    std::optional<std::string> fromBase64(std::string_view text);
}
