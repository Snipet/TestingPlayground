// Source/plugin/Verify.h: the checks that prove a downloaded factory set is one the publisher signed (the website
// repository's docs/factory-projects.md, "Trust"): SHA-256, and ECDSA P-256 over SHA-256 verified against a public key
// the plugin has built in (plugin/FactoryKeys.h). JUCE-free: std, the DSP library's SHA-256 (dsp/Export.h) and each
// system's own library for the signature, so that no new dependency comes in and the code can be read on its own.
//
//   macOS     Security.framework: SecKeyCreateWithData over the X9.63 point (an in-memory key: the Keychain is never
//             touched), SecKeyVerifySignature over the digest with the signature DER-encoded (the format is r then s;
//             Security takes DER)
//   Windows   CNG: BCryptImportKeyPair with a BCRYPT_ECCPUBLIC_BLOB (x, y), BCryptVerifySignature over the digest
//             with r then s as they are
//   Linux     OpenSSL's libcrypto (3 and later, through interfaces none of them deprecates): EVP_PKEY_fromdata with the
//             group's name and the point, EVP_PKEY_verify over the digest with the signature DER-encoded
//
// Each library parses and checks the point (on the curve) and the signature itself, and they differ in what else they
// take: a compressed or hybrid point, a signature of another length, r or s past the curve's order. So p256() refuses
// all of that itself before it calls one, and every platform answers the same for the same bytes. The probe
// plugin.verify holds all three to the known answers of Tools/probes/fixtures/factory/vectors.json.
//
// The digest is the same on every platform (the DSP library's FIPS 180-4 code), so the hash a file is checked by and
// the one a signature is verified over never differ between them. Any thread; nothing kept between calls.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace playground::verify
{
    using Sha256 = std::array<uint8_t, 32>;

    inline constexpr std::size_t kPointBytes = 65;       // 04, x, y: the curve's point, uncompressed (X9.63)
    inline constexpr std::size_t kSignatureBytes = 64;   // r then s, 32 bytes each, big-endian

    Sha256 sha256(const void* data, std::size_t size);
    std::string hex(const Sha256& hash);                 // lower case, 64 characters

    // True when `signature` is an ECDSA P-256 signature over SHA-256 of `message` by the key whose public half is
    // `point`. False for anything else: a point that is not 65 bytes starting 04 or not on the curve, a signature that
    // is not 64 bytes, r or s zero or not below the curve's order, or a signature that does not verify. It never
    // throws. A signature whose s is in the upper half of the order verifies, as ECDSA allows.
    bool p256(std::span<const uint8_t> point, std::span<const uint8_t> message, std::span<const uint8_t> signature);

    // True when `point` is a public key p256() can verify with: 65 bytes starting 04, a point on the curve as this
    // system's library imports it. For checking the keys built in (plugin/FactoryKeys.h) before they are needed.
    bool p256Point(std::span<const uint8_t> point);
}
