// Source/plugin/AssetFiles.cpp: see AssetFiles.h.
#include "plugin/AssetFiles.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>

namespace playground::assetfiles
{
    namespace
    {
        std::atomic<uint64_t> decoded{ 0 };
    }

    std::optional<assets::Audio> decode(const std::string& bytes, std::string_view name, std::string& why)
    {
        decoded.fetch_add(1, std::memory_order_relaxed);
        const std::string n(name);
        juce::AudioFormatManager formats;
        formats.registerFormat(new juce::WavAudioFormat(), true);
        formats.registerFormat(new juce::AiffAudioFormat(), false);
        formats.registerFormat(new juce::FlacAudioFormat(), false);
        auto stream = std::make_unique<juce::MemoryInputStream>(bytes.data(), bytes.size(), false);
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(std::move(stream)));
        if (reader == nullptr)
        {
            why = n + " is not a WAV, AIFF or FLAC file this plugin can read";
            return std::nullopt;
        }
        assets::Audio a;
        a.rate = reader->sampleRate;
        a.channels = static_cast<int>(reader->numChannels);
        const int64_t length = reader->lengthInSamples;
        // Refused before it is read: a header that claims more than a sample may hold reads nothing.
        if (length <= 0 || a.channels <= 0 || !(a.rate > 0.0) || a.channels > assets::kMaxChannels
            || static_cast<double>(length) > assets::kMaxSeconds * a.rate + 1.0)
        {
            a.frames = length > 0 && length < (int64_t{ 1 } << 30) ? static_cast<int>(length) : (length > 0 ? 1 << 30 : 0);
            why = assets::refusal(name, a);
            if (why.empty())
                why = n + " holds no sound";
            return std::nullopt;
        }
        a.frames = static_cast<int>(length);
        juce::AudioBuffer<float> buffer(a.channels, a.frames);
        if (!reader->read(&buffer, 0, a.frames, 0, true, a.channels > 1))
        {
            why = n + " could not be read to its end (a damaged file?)";
            return std::nullopt;
        }
        a.data.resize(static_cast<std::size_t>(a.channels) * static_cast<std::size_t>(a.frames));
        for (int c = 0; c < a.channels; ++c)
            std::copy_n(buffer.getReadPointer(c), a.frames,
                        a.data.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(c) * static_cast<std::size_t>(a.frames)));
        if (const std::string refused = assets::refusal(name, a); !refused.empty())
        {
            why = refused;
            return std::nullopt;
        }
        return a;
    }

    std::shared_ptr<const assets::Asset> make(const assets::Set* to, const std::string& name, std::string bytes, std::string& why)
    {
        if (const std::string refused = assets::refusal(to, name, bytes.size()); !refused.empty())
        {
            why = refused;
            return nullptr;
        }
        std::optional<assets::Audio> audio = decode(bytes, name, why);
        if (!audio)
            return nullptr;
        return std::make_shared<const assets::Asset>(name, std::move(bytes), std::move(*audio));
    }

    std::shared_ptr<const assets::Asset> read(const assets::Set* to, const std::string& path, std::string name, std::string& why)
    {
        const juce::File file(juce::String::fromUTF8(path.c_str()));
        const std::string own = file.getFileName().toStdString();
        if (name.empty())
            name = assets::tidyName(own);
        if (name.empty())
        {
            why = own + " is not a WAV, AIFF or FLAC file (.wav, .aif, .aiff or .flac): a sample is one of those";
            return nullptr;
        }
        if (!file.existsAsFile())
        {
            why = "there is no file at " + path;
            return nullptr;
        }
        const int64_t size = file.getSize();
        if (const std::string refused = assets::refusal(to, name, static_cast<std::size_t>(std::max<int64_t>(0, size)));
            !refused.empty())
        {
            why = refused;
            return nullptr;
        }
        juce::MemoryBlock block;
        if (!file.loadFileAsData(block) || static_cast<int64_t>(block.getSize()) != size)
        {
            why = own + " could not be read";
            return nullptr;
        }
        return make(to, name, std::string(static_cast<const char*>(block.getData()), block.getSize()), why);
    }

    uint64_t decodes() noexcept
    {
        return decoded.load(std::memory_order_relaxed);
    }

    std::string toBase64(const std::string& bytes)
    {
        return juce::Base64::toBase64(bytes.data(), bytes.size()).toStdString();
    }

    std::optional<std::string> fromBase64(std::string_view text)
    {
        juce::MemoryOutputStream out;
        if (!juce::Base64::convertFromBase64(out, juce::StringRef(juce::String(text.data(), text.size()).toRawUTF8())))
            return std::nullopt;
        return std::string(static_cast<const char*>(out.getData()), out.getDataSize());
    }
}
