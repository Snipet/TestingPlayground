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
#include <utility>

namespace testingplayground::chatimages
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

        juce::Image decode(const uint8_t* data, std::size_t size)
        {
            juce::MemoryInputStream in(data, size, false);
            return juce::ImageFileFormat::loadFrom(in);   // JUCE's PNG, JPEG and GIF
        }

#if defined(__APPLE__)
        // ImageIO: any format the system reads, at most `edge` px on its long side, turned upright by its EXIF
        // orientation. `width` and `height` are the file's own size; `turned`, whether it had to be turned.
        juce::Image decodeSystem(const uint8_t* data, std::size_t size, int edge, int& width, int& height, bool& turned)
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
            if (width <= 0 || height <= 0)
            {
                width = w;
                height = h;
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
#if defined(__APPLE__)
        if (!bytes.empty())
            sized = decodeSystem(bytes.data(), bytes.size(), kMaxEdge, fileW, fileH, turned);
#endif
        if (!sized.isValid() && !bytes.empty())
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
            out.why = name + " is not a picture this plugin can read (PNG, JPEG, GIF, WebP, HEIC, TIFF).";
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
}
