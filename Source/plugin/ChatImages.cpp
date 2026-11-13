#include "plugin/ChatImages.h"

#include <juce_graphics/juce_graphics.h>

#if defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <CoreGraphics/CoreGraphics.h>
    #include <ImageIO/ImageIO.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace playground::chatimages
{
    namespace
    {
        std::atomic<uint32_t> lastId{ 0x5c000000u };

        bool startsWith(const std::vector<uint8_t>& b, std::initializer_list<uint8_t> sig)
        {
            if (b.size() < sig.size())
                return false;
            std::size_t i = 0;
            for (const uint8_t s : sig)
                if (b[i++] != s)
                    return false;
            return true;
        }

        bool isPng(const std::vector<uint8_t>& b) { return startsWith(b, { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a }); }
        bool isJpeg(const std::vector<uint8_t>& b) { return startsWith(b, { 0xff, 0xd8, 0xff }); }

        // At most `edge` px on its long side, its proportions kept.
        juce::Image within(const juce::Image& img, int edge)
        {
            const int w = img.getWidth(), h = img.getHeight();
            const int longSide = std::max(w, h);
            if (longSide <= edge)
                return img;
            const double s = static_cast<double>(edge) / static_cast<double>(longSide);
            const int nw = std::max(1, static_cast<int>(std::lround(w * s)));
            const int nh = std::max(1, static_cast<int>(std::lround(h * s)));
            return img.rescaled(nw, nh, juce::Graphics::highResamplingQuality);
        }

        bool transparent(const juce::Image& img)
        {
            if (!img.hasAlphaChannel())
                return false;
            const juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
            for (int y = 0; y < bd.height; ++y)
                for (int x = 0; x < bd.width; ++x)
                    if (bd.getPixelColour(x, y).getAlpha() < 255)
                        return true;
            return false;
        }

        // On white: a JPEG has no transparency.
        juce::Image opaque(const juce::Image& img)
        {
            if (!img.hasAlphaChannel())
                return img;
            juce::Image out(juce::Image::RGB, img.getWidth(), img.getHeight(), true);
            juce::Graphics g(out);
            g.fillAll(juce::Colours::white);
            g.drawImageAt(img, 0, 0);
            return out;
        }

        std::vector<uint8_t> encode(const juce::Image& img, bool png, float quality)
        {
            juce::MemoryOutputStream out;
            if (png)
            {
                juce::PNGImageFormat f;
                f.writeImageToStream(img, out);
            }
            else
            {
                juce::JPEGImageFormat f;
                f.setQuality(quality);
                f.writeImageToStream(opaque(img), out);
            }
            const auto* p = static_cast<const uint8_t*>(out.getData());
            return { p, p + out.getDataSize() };
        }

        // Straight RGBA8, row by row.
        std::vector<uint8_t> pixels(const juce::Image& img)
        {
            const juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
            std::vector<uint8_t> out(static_cast<std::size_t>(bd.width) * static_cast<std::size_t>(bd.height) * 4u);
            std::size_t i = 0;
            for (int y = 0; y < bd.height; ++y)
                for (int x = 0; x < bd.width; ++x)
                {
                    const juce::Colour c = bd.getPixelColour(x, y);
                    out[i++] = c.getRed();
                    out[i++] = c.getGreen();
                    out[i++] = c.getBlue();
                    out[i++] = c.getAlpha();
                }
            return out;
        }

        std::atomic<std::size_t> decoded{ 0 };

        // ---- A picture's size from its header, before anything decodes it ------------------------------------------
        // What JUCE's loaders take a stream for (ImageFileFormat::findImageFormatForStream, in its order: "PNG" at its
        // second byte, then FF D8 FF, then "GIF"), read as their decoders read it, so that no bytes make a decoder size
        // the picture from other bytes than these readers do. Each must stay in step with the code it follows (JUCE
        // 8.0.4's juce_graphics/image_formats: juce_GIFLoader.cpp, jpglib/jdmarker.c, pnglib/pngread.c and
        // pngrutil.c): where the decoder ends with no picture these may refuse, never the other way. Every read is
        // bounded by the bytes.
        struct HeaderSize
        {
            enum class Kind { other, unreadable, sized } kind = Kind::other;
            const char* format = "";
            uint64_t width = 0, height = 0;
        };

        uint32_t be16(const std::vector<uint8_t>& b, std::size_t at) { return (uint32_t{ b[at] } << 8) | b[at + 1]; }
        uint32_t le16(const std::vector<uint8_t>& b, std::size_t at) { return uint32_t{ b[at] } | (uint32_t{ b[at + 1] } << 8); }

        bool overPixels(uint64_t w, uint64_t h) { return w > kMaxPixels || h > kMaxPixels || w * h > kMaxPixels; }

        // libpng (png_read_info): the signature, then the IHDR chunk, whose width and height size the picture. A file
        // whose first chunk is another (libpng reads an unknown one, then a later IHDR) is refused here.
        HeaderSize pngSize(const std::vector<uint8_t>& b)
        {
            HeaderSize s{ HeaderSize::Kind::unreadable, "PNG" };
            if (!isPng(b) || b.size() < 24 || b[12] != 'I' || b[13] != 'H' || b[14] != 'D' || b[15] != 'R')
                return s;
            s.width = (uint64_t{ be16(b, 16) } << 16) | be16(b, 18);
            s.height = (uint64_t{ be16(b, 20) } << 16) | be16(b, 22);
            if (s.width > 0 && s.height > 0)
                s.kind = HeaderSize::Kind::sized;
            return s;
        }

        // libjpeg's read_markers: SOI, then markers found as next_marker finds them (bytes up to an FF passed over, FF
        // fill, FF 00 passed over), a segment skipped by its length (APPn, COM, DNL, DAC: skip_variable or
        // get_interesting_appn; DHT, DQT and DRI consume exactly their length or fail), up to the first SOFn, whose
        // height and width size the picture. Any error (a second SOF, SOS or EOI before one, an unknown marker) is
        // marked by JUCE's silent error handler, and JUCE then makes no picture. A length under 2 (which libjpeg skips
        // as 2) and data that ends inside a segment are refused here.
        HeaderSize jpegSize(const std::vector<uint8_t>& b)
        {
            HeaderSize s{ HeaderSize::Kind::unreadable, "JPEG" };
            std::size_t at = 2;                              // past SOI
            while (at < b.size())
            {
                while (at < b.size() && b[at] != 0xff)       // what is no marker (libjpeg passes over it)
                    ++at;
                while (at < b.size() && b[at] == 0xff)       // fill bytes
                    ++at;
                if (at >= b.size())
                    break;
                const uint8_t m = b[at++];
                if (m == 0x00 || m == 0x01 || (m >= 0xd0 && m <= 0xd8))
                    continue;                                // a stuffed byte, TEM, RSTn, SOI: no length
                if (m == 0xd9 || m == 0xda)
                    break;                                   // EOI, or SOS before any frame: no picture
                if (at + 2 > b.size())
                    break;
                const std::size_t length = be16(b, at);
                if (length < 2)
                    break;
                const bool sof = m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc;
                if (sof)
                {
                    if (length < 7 || at + 7 > b.size())
                        break;
                    s.height = be16(b, at + 3);
                    s.width = be16(b, at + 5);
                    if (s.width > 0 && s.height > 0)         // a height of 0 (a DNL marker's later) is none here
                        s.kind = HeaderSize::Kind::sized;
                    return s;
                }
                at += length;
            }
            return s;
        }

        // JUCE's GIFLoader, step for step over the bytes as its MemoryInputStream gives them (a read takes what is
        // left, at most what it asks): the header and the logical screen, its three bytes, the palette, then its main
        // loop (';' ends it, '!' an extension as readExtension reads one, ',' the image descriptor whose width and
        // height size the picture, any other byte passed over). readExtension of a graphic control extension (0xf9)
        // reads one data block first whatever its size, 0 included, then data blocks until one of size 0. The logical
        // screen's size is refused past the cap too (macOS's loader may size the picture by it).
        HeaderSize gifSize(const std::vector<uint8_t>& b)
        {
            HeaderSize s{ HeaderSize::Kind::unreadable, "GIF" };
            std::size_t at = 0;
            const auto read = [&](std::size_t k) {
                const std::size_t n = std::min(k, b.size() - at);
                at += n;
                return n;
            };
            const auto readDataBlock = [&]() -> int {
                if (read(1) != 1)
                    return -1;
                const std::size_t n = b[at - 1];
                if (n == 0 || read(n) == n)
                    return static_cast<int>(n);
                return -1;
            };
            const auto readExtension = [&]() -> bool {
                if (read(1) != 1)
                    return false;
                if (b[at - 1] == 0xf9 && readDataBlock() < 0)
                    return true;
                int n = 0;
                do
                    n = readDataBlock();
                while (n > 0);
                return n >= 0;
            };
            if (read(6) != 6 || (std::memcmp(b.data(), "GIF87a", 6) != 0 && std::memcmp(b.data(), "GIF89a", 6) != 0)
                || read(4) != 4)
                return s;
            const uint64_t screenW = le16(b, 6), screenH = le16(b, 8);
            if (screenW == 0 || screenH == 0 || read(3) != 3)
                return s;                                    // JUCE's loader makes no picture
            if ((b[10] & 0x80) != 0)
                for (int i = 0, n = 2 << (b[10] & 7); i < n; ++i)
                    (void)read(3);                           // the global palette, a short read passed over as JUCE does
            for (;;)
            {
                if (read(1) != 1 || b[at - 1] == ';')
                    return s;
                const uint8_t c = b[at - 1];
                if (c == '!')
                {
                    if (readExtension())
                        continue;
                    return s;
                }
                if (c != ',')
                    continue;
                if (read(9) != 9)
                    return s;
                s.width = le16(b, at - 9 + 4);
                s.height = le16(b, at - 9 + 6);
                if (s.width > 0 && s.height > 0)
                    s.kind = HeaderSize::Kind::sized;
                if (s.kind == HeaderSize::Kind::sized && !overPixels(s.width, s.height) && overPixels(screenW, screenH))
                {
                    s.width = screenW;
                    s.height = screenH;
                }
                return s;
            }
        }

        HeaderSize headerSize(const std::vector<uint8_t>& b)
        {
            if (b.size() >= 4 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
                return pngSize(b);
            if (b.size() >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff)
                return jpegSize(b);
            if (b.size() >= 3 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F')
                return gifSize(b);
            return {};
        }

        std::string tooLarge(const std::string& name, uint64_t w, uint64_t h)
        {
            return name + " is " + std::to_string(w) + " x " + std::to_string(h) + " pixels: a picture of more than "
                 + std::to_string(kMaxPixels / 1'000'000) + " megapixels is not read (make it smaller first).";
        }

        juce::Image decode(const uint8_t* data, std::size_t size)
        {
            decoded.fetch_add(1, std::memory_order_relaxed);
            juce::MemoryInputStream in(data, size, false);
            return juce::ImageFileFormat::loadFrom(in);   // JUCE's PNG, JPEG and GIF
        }

#if defined(__APPLE__)
        // ImageIO: any format the system reads, at most `edge` px on its long side, turned upright by its EXIF
        // orientation. `width` and `height` are the file's own size; `turned`, whether it had to be turned.
        juce::Image decodeSystem(const uint8_t* data, std::size_t size, int edge, int& width, int& height, bool& turned,
                                 std::string& refused, const std::string& name)
        {
            width = height = 0;
            turned = false;
            CFDataRef bytes = CFDataCreate(nullptr, data, static_cast<CFIndex>(size));
            if (bytes == nullptr)
                return {};
            CGImageSourceRef src = CGImageSourceCreateWithData(bytes, nullptr);
            CFRelease(bytes);
            if (src == nullptr)
                return {};
            if (CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr))
            {
                const auto number = [&](CFStringRef key) {
                    int v = 0;
                    if (const auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(props, key)))
                        CFNumberGetValue(n, kCFNumberIntType, &v);
                    return v;
                };
                width = number(kCGImagePropertyPixelWidth);
                height = number(kCGImagePropertyPixelHeight);
                const int orientation = number(kCGImagePropertyOrientation);
                turned = orientation > 1;
                if (orientation >= 5)
                    std::swap(width, height);         // turned a quarter: its upright size
                CFRelease(props);
            }
            // Its size before it decodes (kMaxPixels): none said (nothing it reads), or too many, and nothing is decoded.
            if (width <= 0 || height <= 0 || overPixels(static_cast<uint64_t>(width), static_cast<uint64_t>(height)))
            {
                if (width > 0 && height > 0)
                    refused = tooLarge(name, static_cast<uint64_t>(width), static_cast<uint64_t>(height));
                CFRelease(src);
                return {};
            }
            decoded.fetch_add(1, std::memory_order_relaxed);
            const int maxSide = edge;
            CFNumberRef max = CFNumberCreate(nullptr, kCFNumberIntType, &maxSide);
            const void* keys[] = { kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform,
                                   kCGImageSourceThumbnailMaxPixelSize };
            const void* values[] = { kCFBooleanTrue, kCFBooleanTrue, max };
            CFDictionaryRef opts = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks);
            CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts);
            CFRelease(opts);
            CFRelease(max);
            CFRelease(src);
            if (img == nullptr)
                return {};
            const int w = static_cast<int>(CGImageGetWidth(img)), h = static_cast<int>(CGImageGetHeight(img));
            if (w < 1 || h < 1)
            {
                CGImageRelease(img);
                return {};
            }
            juce::Image out(juce::Image::ARGB, w, h, true);
            {
                // JUCE's ARGB is premultiplied BGRA in memory: CoreGraphics' premultiplied-first, little-endian.
                juce::Image::BitmapData bd(out, juce::Image::BitmapData::writeOnly);
                CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                CGContextRef ctx = CGBitmapContextCreate(bd.data, static_cast<size_t>(w), static_cast<size_t>(h), 8,
                                                         static_cast<size_t>(bd.lineStride), cs,
                                                         static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst)
                                                             | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
                CGColorSpaceRelease(cs);
                if (ctx != nullptr)
                {
                    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
                    CGContextRelease(ctx);
                }
            }
            CGImageRelease(img);
            return out;
        }
#endif
    }

    uint32_t nextId() { return lastId.fetch_add(1, std::memory_order_relaxed) + 1; }

    std::size_t decodes() { return decoded.load(std::memory_order_relaxed); }

    Prepared fromFile(const std::string& path)
    {
        Prepared out;
        const juce::File f(juce::String::fromUTF8(path.c_str()));
        const std::string name = f.getFileName().toStdString();
        if (!f.existsAsFile())
        {
            out.why = name + " is not there.";
            return out;
        }
        if (static_cast<std::size_t>(f.getSize()) > kMaxFileBytes)
        {
            out.why = name + " is larger than " + std::to_string(kMaxFileBytes >> 20) + " MB.";
            return out;
        }
        juce::MemoryBlock block;
        if (!f.loadFileAsData(block))
        {
            out.why = name + " could not be read.";
            return out;
        }
        const auto* p = static_cast<const uint8_t*>(block.getData());
        return fromBytes({ p, p + block.getSize() }, name);
    }

    Prepared fromBytes(const std::vector<uint8_t>& bytes, const std::string& name)
    {
        Prepared out;
        // The file's own size, and whether it had to be turned upright (then its bytes cannot go as they are).
        int fileW = 0, fileH = 0;
        bool turned = false;
        juce::Image sized;
        // Its size from its header, before any decoder sees it (kMaxPixels).
        const HeaderSize header = headerSize(bytes);
        if (header.kind == HeaderSize::Kind::unreadable)
        {
            out.why = name + " is not a picture this plugin can read (its " + header.format + " header does not say its size).";
            return out;
        }
        if (header.kind == HeaderSize::Kind::sized && overPixels(header.width, header.height))
        {
            out.why = tooLarge(name, header.width, header.height);
            return out;
        }
#if defined(__APPLE__)
        if (!bytes.empty())
        {
            sized = decodeSystem(bytes.data(), bytes.size(), kMaxEdge, fileW, fileH, turned, out.why, name);
            if (!out.why.empty())
                return out;
        }
#endif
        if (!sized.isValid() && header.kind == HeaderSize::Kind::sized)   // JUCE's loaders read these three alone
        {
            const juce::Image img = decode(bytes.data(), bytes.size());
            if (img.isValid())
            {
                fileW = img.getWidth();
                fileH = img.getHeight();
                sized = within(img, kMaxEdge);
            }
        }
        if (!sized.isValid() || sized.getWidth() < 1 || sized.getHeight() < 1)
        {
            out.why = name + " is not a picture this plugin can read (" + kFormats + ").";
            return out;
        }
        const bool kept = !turned && sized.getWidth() == fileW && sized.getHeight() == fileH && bytes.size() <= kMaxBytes
                       && (isPng(bytes) || isJpeg(bytes));
        if (kept)
        {
            out.data = bytes;
            out.mediaType = isPng(bytes) ? "image/png" : "image/jpeg";
        }
        else
        {
            if (transparent(sized))
            {
                out.data = encode(sized, true, 0.0f);
                out.mediaType = "image/png";
            }
            // Too large as a PNG, or opaque: a JPEG, smaller (and smaller again) until it fits.
            for (int step = 0; (out.data.empty() || out.data.size() > kMaxBytes) && step < 4; ++step)
            {
                if (step == 3)
                    sized = within(sized, 1024);
                out.data = encode(sized, false, step == 0 ? 0.9f : step == 1 ? 0.8f : 0.65f);
                out.mediaType = "image/jpeg";
            }
            if (out.data.size() > kMaxBytes)
            {
                out.why = name + " is too large to send, even as a smaller JPEG.";
                out.data.clear();
                return out;
            }
        }
        auto picture = std::make_shared<ChatPicture>();
        picture->id = nextId();
        picture->name = name;
        picture->width = sized.getWidth();
        picture->height = sized.getHeight();
        const juce::Image thumb = within(sized, kThumbEdge);
        picture->thumbWidth = thumb.getWidth();
        picture->thumbHeight = thumb.getHeight();
        picture->thumb = pixels(thumb);
        const bool alpha = transparent(thumb);
        picture->thumbData = encode(thumb, alpha, 0.85f);
        picture->thumbType = alpha ? "image/png" : "image/jpeg";
        out.picture = std::move(picture);
        return out;
    }

    bool decodeThumb(ChatPicture& picture)
    {
        if (picture.thumbData.empty())
            return false;
        const juce::Image img = decode(picture.thumbData.data(), picture.thumbData.size());
        if (!img.isValid())
            return false;
        picture.thumbWidth = img.getWidth();
        picture.thumbHeight = img.getHeight();
        picture.thumb = pixels(img);
        return true;
    }

    Fitted fit(const std::vector<uint8_t>& picture, std::size_t maxBytes)
    {
        Fitted out;
        out.data = picture;
        out.mediaType = isJpeg(picture) ? "image/jpeg" : "image/png";
        const juce::Image img = decode(picture.data(), picture.size());
        out.width = out.fromWidth = img.isValid() ? img.getWidth() : 0;
        out.height = out.fromHeight = img.isValid() ? img.getHeight() : 0;
        if (picture.size() <= maxBytes || !img.isValid())
            return out;
        out.changed = true;
        // The largest picture that fits, each format sized from its own bytes: an encoding's size goes with its area,
        // so the long side that should fit, a little under (and a step down at least). A JPEG first, from the whole
        // picture: a sound's (a spectrogram's noise) is several times smaller as a JPEG than as a PNG, so sizing it
        // from the PNG's bytes sent a third of its width. Then a PNG at the size its bytes say, when that is larger
        // still and fits (a flat picture: a panel's).
        const int full = std::max(img.getWidth(), img.getHeight());
        const auto smallerFor = [&](int edge, std::size_t bytes) {
            const double s = std::min(0.9, 0.95 * std::sqrt(static_cast<double>(maxBytes) / static_cast<double>(bytes)));
            return std::max(64, static_cast<int>(std::lround(edge * s)));
        };
        int edge = full;
        juce::Image smaller = img;
        out.data = encode(smaller, false, 0.85f);
        for (int step = 0; out.data.size() > maxBytes && edge > 64 && step < 12; ++step)
        {
            edge = smallerFor(edge, out.data.size());
            smaller = within(img, edge);
            out.data = encode(smaller, false, step < 8 ? 0.85f : 0.7f);   // smaller, then lower in quality too
        }
        out.mediaType = "image/jpeg";
        const int pngEdge = smallerFor(full, picture.size());
        if (pngEdge > edge || out.data.size() > maxBytes)
        {
            juce::Image png = within(img, pngEdge);
            if (std::vector<uint8_t> data = encode(png, true, 0.0f); data.size() <= maxBytes)
            {
                out.data = std::move(data);
                out.mediaType = "image/png";
                smaller = png;
            }
        }
        out.width = smaller.getWidth();
        out.height = smaller.getHeight();
        return out;
    }
}
