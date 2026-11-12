// Source/plugin/Verify.cpp: see Verify.h.
#include "plugin/Verify.h"

#include "dsp/Export.h"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

namespace playground::verify
{
    namespace
    {
        constexpr std::size_t kScalarBytes = 32;

        // P-256's order, n (SEC 2, section 2.4.2), big-endian: r and s are each in [1, n - 1].
        constexpr std::array<uint8_t, kScalarBytes> kOrder = {
            0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
        };

        // A scalar of the signature (32 bytes, big-endian) in [1, n - 1]. The libraries check this too, but not all in
        // the same way (one reduces, one refuses), so it is decided here, once, for every platform.
        bool inRange(const uint8_t* v)
        {
            bool zero = true;
            for (std::size_t i = 0; i < kScalarBytes; ++i)
                zero = zero && v[i] == 0;
            if (zero)
                return false;
            for (std::size_t i = 0; i < kScalarBytes; ++i)
                if (v[i] != kOrder[i])
                    return v[i] < kOrder[i];
            return false;                                // equal to n
        }

        int hexDigit(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return -1;
        }

#if !defined(_WIN32)
        // r then s as DER (X9.62's ECDSA-Sig-Value: a SEQUENCE of two INTEGERs), which Security.framework and OpenSSL
        // take. Each INTEGER is minimal: its leading zero bytes dropped, and one 00 put back when the first byte left has
        // its top bit set (else it would read as negative). Both scalars are nonzero here (inRange), so each keeps at
        // least one byte; the content is at most 2 x 35 bytes, so every length fits the short form.
        std::vector<uint8_t> der(const uint8_t* rs)
        {
            std::vector<uint8_t> out{ 0x30, 0 };
            for (const uint8_t* v : { rs, rs + kScalarBytes })
            {
                std::size_t skip = 0;
                while (skip < kScalarBytes - 1 && v[skip] == 0)
                    ++skip;
                const bool pad = (v[skip] & 0x80) != 0;
                out.push_back(0x02);
                out.push_back(static_cast<uint8_t>(kScalarBytes - skip + (pad ? 1 : 0)));
                if (pad)
                    out.push_back(0);
                out.insert(out.end(), v + skip, v + kScalarBytes);
            }
            out[1] = static_cast<uint8_t>(out.size() - 2);
            return out;
        }
#endif

        // ---- the key and the signature over a digest, by each system's library ---------------------------------------
        // keyFrom: the library's key from a point (65 bytes, 04 already checked), or null when it refuses the point (one
        // off the curve); verifyDigest: the signature (r then s, in range) over the digest, by that key.
#if defined(_WIN32)
        struct Key
        {
            BCRYPT_ALG_HANDLE alg = nullptr;
            BCRYPT_KEY_HANDLE key = nullptr;
        };

        void release(Key& k)
        {
            if (k.key != nullptr)
                BCryptDestroyKey(k.key);
            if (k.alg != nullptr)
                BCryptCloseAlgorithmProvider(k.alg, 0);
            k = {};
        }

        Key keyFrom(const uint8_t* point)
        {
            Key k;
            if (BCryptOpenAlgorithmProvider(&k.alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0 || k.alg == nullptr)
            {
                k.alg = nullptr;
                return k;
            }
            // BCRYPT_ECCPUBLIC_BLOB: the header (its magic, the size of a coordinate), then x and y, big-endian. CNG
            // checks that the point is on the curve as it imports it (BCRYPT_NO_KEY_VALIDATION is not given).
            BCRYPT_ECCKEY_BLOB head{};
            head.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
            head.cbKey = static_cast<ULONG>(kScalarBytes);
            std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kScalarBytes> blob{};
            std::memcpy(blob.data(), &head, sizeof head);
            std::memcpy(blob.data() + sizeof head, point + 1, 2 * kScalarBytes);
            if (BCryptImportKeyPair(k.alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &k.key, blob.data(), static_cast<ULONG>(blob.size()), 0) != 0)
                k.key = nullptr;
            return k;
        }

        bool usable(const Key& k) { return k.key != nullptr; }

        bool verifyDigest(const Key& k, const Sha256& digest, const uint8_t* rs)
        {
            // CNG's ECDSA signature is r then s, each a coordinate's size: the format's own bytes.
            Sha256 hash = digest;
            std::array<uint8_t, kSignatureBytes> signature{};
            std::memcpy(signature.data(), rs, kSignatureBytes);
            return BCryptVerifySignature(k.key, nullptr, hash.data(), static_cast<ULONG>(hash.size()), signature.data(),
                                         static_cast<ULONG>(signature.size()), 0)
                == 0;
        }
#elif defined(__APPLE__)
        using Key = SecKeyRef;

        void release(Key& k)
        {
            if (k != nullptr)
                CFRelease(k);
            k = nullptr;
        }

        Key keyFrom(const uint8_t* point)
        {
            // An in-memory key (no Keychain): SecKeyCreateWithData takes the X9.63 point and refuses one off the curve.
            int bits = 256;
            CFDataRef data = CFDataCreate(kCFAllocatorDefault, point, static_cast<CFIndex>(kPointBytes));
            CFNumberRef size = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bits);
            CFDictionaryRef attributes = nullptr;
            if (size != nullptr)
            {
                const void* names[] = { kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits };
                const void* values[] = { kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic, size };
                attributes = CFDictionaryCreate(kCFAllocatorDefault, names, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
            }
            Key key = data != nullptr && attributes != nullptr ? SecKeyCreateWithData(data, attributes, nullptr) : nullptr;
            for (CFTypeRef each : { static_cast<CFTypeRef>(data), static_cast<CFTypeRef>(size), static_cast<CFTypeRef>(attributes) })
                if (each != nullptr)
                    CFRelease(each);
            return key;
        }

        bool usable(const Key& k) { return k != nullptr; }

        bool verifyDigest(const Key& k, const Sha256& digest, const uint8_t* rs)
        {
            const std::vector<uint8_t> signature = der(rs);
            CFDataRef digestData = CFDataCreate(kCFAllocatorDefault, digest.data(), static_cast<CFIndex>(digest.size()));
            CFDataRef signatureData = CFDataCreate(kCFAllocatorDefault, signature.data(), static_cast<CFIndex>(signature.size()));
            const bool ok = digestData != nullptr && signatureData != nullptr
                         && SecKeyVerifySignature(k, kSecKeyAlgorithmECDSASignatureDigestX962SHA256, digestData, signatureData,
                                                  nullptr);
            for (CFTypeRef each : { static_cast<CFTypeRef>(digestData), static_cast<CFTypeRef>(signatureData) })
                if (each != nullptr)
                    CFRelease(each);
            return ok;
        }
#else
        using Key = EVP_PKEY*;

        void release(Key& k)
        {
            EVP_PKEY_free(k);
            k = nullptr;
            ERR_clear_error();                           // a refusal leaves its reasons on this thread's queue
        }

        Key keyFrom(const uint8_t* point)
        {
            // The key from its parameters: the group by name and the point, which EC_POINT_oct2point checks is on the
            // curve. OSSL_PARAM takes its buffers as non-const; neither is written.
            std::array<unsigned char, kPointBytes> pub{};
            std::copy(point, point + kPointBytes, pub.begin());
            char group[] = "prime256v1";
            std::array<OSSL_PARAM, 3> params = {
                OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
                OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, pub.data(), pub.size()),
                OSSL_PARAM_construct_end(),
            };
            Key key = nullptr;
            EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
            if (ctx == nullptr || EVP_PKEY_fromdata_init(ctx) != 1
                || EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params.data()) != 1)
            {
                EVP_PKEY_free(key);
                key = nullptr;
            }
            EVP_PKEY_CTX_free(ctx);
            return key;
        }

        bool usable(const Key& k) { return k != nullptr; }

        bool verifyDigest(const Key& k, const Sha256& digest, const uint8_t* rs)
        {
            const std::vector<uint8_t> signature = der(rs);
            EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_pkey(nullptr, k, nullptr);
            const bool ok = ctx != nullptr && EVP_PKEY_verify_init(ctx) == 1 && EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) == 1
                         && EVP_PKEY_verify(ctx, signature.data(), signature.size(), digest.data(), digest.size()) == 1;
            EVP_PKEY_CTX_free(ctx);
            return ok;
        }
#endif
    }

    Sha256 sha256(const void* data, std::size_t size)
    {
        measure::Sha256 h;
        h.add(data, size);
        const std::string digits = h.hex();
        Sha256 out{};
        for (std::size_t i = 0; i < out.size() && 2 * i + 1 < digits.size(); ++i)
            out[i] = static_cast<uint8_t>(hexDigit(digits[2 * i]) << 4 | hexDigit(digits[2 * i + 1]));
        return out;
    }

    std::string hex(const Sha256& hash)
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out;
        out.reserve(2 * hash.size());
        for (const uint8_t b : hash)
        {
            out += kDigits[b >> 4];
            out += kDigits[b & 15];
        }
        return out;
    }

    bool p256Point(std::span<const uint8_t> point)
    {
        if (point.size() != kPointBytes || point[0] != 0x04)
            return false;
        Key key = keyFrom(point.data());
        const bool ok = usable(key);
        release(key);
        return ok;
    }

    bool p256(std::span<const uint8_t> point, std::span<const uint8_t> message, std::span<const uint8_t> signature)
    {
        if (point.size() != kPointBytes || point[0] != 0x04 || signature.size() != kSignatureBytes)
            return false;
        if (!inRange(signature.data()) || !inRange(signature.data() + kScalarBytes))
            return false;
        const Sha256 digest = sha256(message.data(), message.size());
        Key key = keyFrom(point.data());
        const bool ok = usable(key) && verifyDigest(key, digest, signature.data());
        release(key);
        return ok;
    }
}
