// Source/plugin/AssetFiles.cpp: see AssetFiles.h.
#include "plugin/AssetFiles.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
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
        auto* const wav = new juce::WavAudioFormat();
        auto* const aiff = new juce::AiffAudioFormat();
        formats.registerFormat(wav, true);
        formats.registerFormat(aiff, false);
        formats.registerFormat(new juce::FlacAudioFormat(), false);
        auto stream = std::make_unique<juce::MemoryInputStream>(bytes.data(), bytes.size(), false);
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(std::move(stream)));
        if (reader == nullptr)
        {
            why = n + " is not a WAV, AIFF or FLAC file this plugin can read";
            return std::nullopt;
        }
        // What the header claims, refused before anything is read or allocated: JUCE's readers take a header's rate
        // and length as given, and give silence for whatever the bytes lack, so a damaged file of 50 bytes could ask
        // for gigabytes. The rate, the length and the size decoded within the caps (Assets.h's refusal()), and a WAV's
        // or an AIFF's sound (uncompressed: its frames times its bytes a frame) within the bytes after its least header
        // (44 bytes, 54), which a file cut short does not have. A FLAC's size decoded is its cap alone.
        const int64_t length = reader->lengthInSamples;
        const auto channels = static_cast<int64_t>(reader->numChannels);
        if (std::string refused = assets::refusal(name, reader->sampleRate, channels, length); !refused.empty())
        {
            why = std::move(refused);
            return std::nullopt;
        }
        const bool isWav = reader->getFormatName() == wav->getFormatName();
        if (isWav || reader->getFormatName() == aiff->getFormatName())
        {
            const int64_t frameBytes = std::max<int64_t>(1, channels * static_cast<int64_t>(reader->bitsPerSample) / 8);
            const auto room = static_cast<int64_t>(bytes.size()) - (isWav ? 44 : 54);
            if (length > room / frameBytes)
            {
                why = n + " is damaged or cut short: its header names more sound than its "
                    + assets::sizeText(bytes.size()) + " hold";
                return std::nullopt;
            }
        }
        assets::Audio a;
        a.rate = reader->sampleRate;
        a.channels = static_cast<int>(channels);
        a.frames = static_cast<int>(length);
        a.data.resize(static_cast<std::size_t>(a.channels) * static_cast<std::size_t>(a.frames));
        // Planar, straight into the sound: channel c at data + c * frames.
        float* planes[assets::kMaxChannels] = { a.data.data(), a.channels > 1 ? a.data.data() + a.frames : nullptr };
        if (!reader->read(planes, a.channels, 0, a.frames))
        {
            why = n + " could not be read to its end (a damaged file?)";
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
