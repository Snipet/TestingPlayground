// Source/plugin/FactoryKeys.h: the public keys the plugins take a factory index from (plugin/Verify.h; the website
// repository's docs/factory-projects.md, "Trust"). An index names the key that signed it by its id, and a plugin takes
// it only when its signature verifies against the key of that id here; an id not here is refused.
//
// Each key is the curve's point (P-256, 65 bytes: 04, x, y) as the website repository's factory-keys.json holds it,
// in base64 beside the bytes: plugin.verify decodes the base64 and checks it is these bytes, so a typo in either is
// caught. Only public halves are here; the private key stays on the publisher's Mac and never enters a repository.
//
// A key change (the website repository's README, "The signing key") goes over one release: the next key (f2, made
// with `npm run factory-key -- --new f2` there) is added here as a second row while f1 still signs; once a release
// with both is out, the website publishes with f2, and a later release may drop f1. Copy a key from factory-keys.json
// as it is committed there, never from anywhere else.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace playground::factory
{
    struct PublicKey
    {
        std::string_view id;                             // as an index's "key" names it
        std::array<uint8_t, 65> point;                   // 04, x, y
        std::string_view base64;                         // the same, as factory-keys.json has it
    };

    inline constexpr std::array<PublicKey, 1> kPublicKeys = { {
        { "f1",
          { 0x04, 0x8b, 0x75, 0xa1, 0x84, 0x7a, 0xfb, 0xc1, 0xd0, 0xa9, 0x1a, 0xf6, 0x22, 0xf8, 0x67, 0xf6,
            0x59, 0x0e, 0xfa, 0x23, 0x74, 0xb0, 0xad, 0x44, 0x46, 0x0d, 0x04, 0x57, 0xe0, 0xdf, 0x70, 0xb8,
            0xa9, 0xa5, 0x04, 0x7c, 0xf2, 0xb8, 0x2e, 0x5e, 0x50, 0x34, 0x7a, 0x09, 0xc9, 0x0a, 0xb1, 0x3f,
            0xb3, 0x47, 0xcb, 0x37, 0x16, 0xab, 0x94, 0x15, 0x80, 0x4d, 0x4a, 0x04, 0x35, 0xfd, 0x2e, 0x90,
            0xeb },
          "BIt1oYR6+8HQqRr2Ivhn9lkO+iN0sK1ERg0EV+DfcLippQR88rguXlA0egnJCrE/s0fLNxarlBWATUoENf0ukOs=" },
    } };

    // The point of the key with that id, or an empty span when the plugins have no such key.
    constexpr std::span<const uint8_t> publicKey(std::string_view id)
    {
        for (const PublicKey& key : kPublicKeys)
            if (key.id == id)
                return key.point;
        return {};
    }
}
