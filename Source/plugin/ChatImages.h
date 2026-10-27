// Source/plugin/ChatImages.h: the pictures the user adds to a message for Claude (a synth's panel to take after, a
// sketch, a screenshot), read on macOS with ImageIO (PNG, JPEG, GIF, WebP, HEIC, TIFF, BMP; a photo's EXIF orientation
// applied, scaled as it decodes), elsewhere with JUCE (PNG, JPEG, GIF).
// Each is scaled to at most kMaxEdge on its long side (the API's advice: a larger one is scaled down there anyway, at
// the same cost), and sent as the file's own bytes when it is a PNG or a JPEG that needs no scaling and fits kMaxBytes;
// else encoded again, PNG when it has transparency (and fits), JPEG otherwise. A thumbnail goes with it for the chat
// (kThumbEdge on its long side: RGBA8 to draw, and encoded, for the session).
#pragma once

#include "plugin/ChatPort.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace testingplaygroundsynth::chatimages
{
    inline constexpr int kMaxEdge = 1568;                    // px, the long side of a picture as sent
    inline constexpr int kThumbEdge = 320;                   // px, the thumbnail's
    inline constexpr std::size_t kMaxBytes = 3'750'000;      // a picture as sent (5 MB once in base64)
    inline constexpr std::size_t kMaxFileBytes = 64u << 20;  // a file larger is not read
    inline constexpr int kMaxPerMessage = 6;
    inline constexpr int kMaxInChat = 20;                    // in the history: a request carries them all

    struct Prepared
    {
        std::string why;                                     // why it could not be added ("" when it could)
        std::string mediaType;                               // image/png or image/jpeg
        std::vector<uint8_t> data;                           // the picture as sent
        std::shared_ptr<ChatPicture> picture;                // its size, name and thumbnail, for the chat
    };

    Prepared fromFile(const std::string& path);
    Prepared fromBytes(const std::vector<uint8_t>& bytes, const std::string& name);

    // The thumbnail's pixels from its encoded bytes (a session's): false when they do not decode.
    bool decodeThumb(ChatPicture& picture);

    // An id no other picture of this process has (the editor draws a thumbnail by it).
    uint32_t nextId();
}
