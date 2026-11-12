// PROBE name=plugin.verify timeout=60
//
// plugin.verify: the checks a factory set is proven by (plugin/Verify.h), on this platform's library, against the known
// answers the website repository made (Tools/probes/fixtures/factory/: its README says each one), and the keys the
// plugins have built in (plugin/FactoryKeys.h). Nothing here touches the network or a folder of the user's.
//   P256      every vector of vectors.json: p256() answers its `valid` (a valid signature with s in the upper half of
//             the order among them; r or s zero or past the order, compressed and hybrid points, signatures of 63, 65
//             and DER's bytes refused)
//   POINT     p256Point(): each valid vector's point imports, and each malformed one (off the curve, zero, compressed,
//             hybrid, no prefix, 66 bytes, empty) does not
//   SHA256    every known answer of vectors.json (FIPS 180-2's: the empty message, "abc", two blocks, a million 'a';
//             and a megabyte of 'a' and the padding's edges): sha256() then hex() give its digest
//   HEX       hex() of bytes made here: two lower-case digits a byte, 64 in all
//   KEYS      each key built in: 65 bytes starting 04, the very bytes its base64 gives, a point this platform imports;
//             f1 among them
//   FIXTURES  the test keys (keys.json) import, and publicKey() has neither (an id not built in finds nothing); each case's index (cases.json) verifies with the key it names exactly
//             when the case is not refused for its signature; each project file of a case hashes to its own name but
//             where the case is a file of the wrong bytes or size
#include "Probe.h"

#include "plugin/FactoryKeys.h"
#include "plugin/Verify.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    using Bytes = std::vector<uint8_t>;

    juce::File fixtures() { return juce::File(PROBE_SOURCE_DIR).getChildFile("Tools/probes/fixtures/factory"); }

    // Base64 (RFC 4648, as the website writes it) to bytes; false when it is none.
    bool fromBase64(const juce::String& text, Bytes& out)
    {
        juce::MemoryOutputStream stream;
        if (!juce::Base64::convertFromBase64(stream, text))
            return false;
        const auto* p = static_cast<const uint8_t*>(stream.getData());
        out.assign(p, p + stream.getDataSize());
        return true;
    }

    Bytes bytesOf(const juce::var& v)
    {
        Bytes out;
        fromBase64(v.toString(), out);
        return out;
    }

    Bytes bytesOf(const std::string& text) { return Bytes(text.begin(), text.end()); }

    bool readJson(funkgui::test::Probe& P, const juce::File& f, juce::var& out)
    {
        if (!f.existsAsFile())
        {
            P.harnessError("no " + f.getFullPathName().toStdString() + ": the fixtures are made by the website repository");
            return false;
        }
        out = juce::JSON::parse(f.loadFileAsString());
        if (!out.isObject() && !out.isArray())
        {
            P.harnessError(f.getFileName().toStdString() + " does not parse");
            return false;
        }
        return true;
    }

    // A malformed point's vector: the point is refused whatever the signature.
    bool malformedPoint(const std::string& name)
    {
        static const std::set<std::string> kNames = { "point-off-curve", "point-zero", "point-compressed", "point-hybrid",
                                                      "point-no-prefix", "point-66-bytes", "point-empty" };
        return kNames.count(name) != 0;
    }
}

PROBE(plugin, verify)
{
    juce::var vectors, keysJson, cases;
    if (!readJson(P, fixtures().getChildFile("vectors.json"), vectors) || !readJson(P, fixtures().getChildFile("keys.json"), keysJson)
        || !readJson(P, fixtures().getChildFile("cases.json"), cases))
        return P.finish();

    // ---- P256 and POINT ----------------------------------------------------------------------------------------------
    {
        const juce::Array<juce::var>* list = vectors["p256"].getArray();
        int count = 0, valid = 0;
        bool highS = false;
        for (const juce::var& v : list != nullptr ? *list : juce::Array<juce::var>())
        {
            const std::string name = v["name"].toString().toStdString();
            const Bytes point = bytesOf(v["point"]), message = bytesOf(v["message"]), signature = bytesOf(v["signature"]);
            const bool want = static_cast<bool>(v["valid"]);
            P.eq("p256." + name, pg::verify::p256(point, message, signature) ? 1 : 0, want ? 1 : 0);
            if (name.rfind("valid", 0) == 0)
                P.eq("point." + name, pg::verify::p256Point(point) ? 1 : 0, 1);
            else if (malformedPoint(name))
                P.eq("point." + name, pg::verify::p256Point(point) ? 1 : 0, 0);
            ++count;
            valid += want ? 1 : 0;
            highS = highS || (name == "valid-high-s" && want);
        }
        P.ge("p256.vectors", count, 25);
        P.ge("p256.valid_vectors", valid, 5);
        P.eq("p256.high_s_there", highS ? 1 : 0, 1);
    }

    // ---- SHA256 and HEX ----------------------------------------------------------------------------------------------
    {
        const juce::Array<juce::var>* list = vectors["sha256"].getArray();
        int count = 0;
        for (const juce::var& v : list != nullptr ? *list : juce::Array<juce::var>())
        {
            const Bytes piece = bytesOf(v["message"]);
            const int repeat = static_cast<int>(v["repeat"]);
            Bytes message;
            message.reserve(piece.size() * static_cast<std::size_t>(std::max(repeat, 0)));
            for (int i = 0; i < repeat; ++i)
                message.insert(message.end(), piece.begin(), piece.end());
            const std::string got = pg::verify::hex(pg::verify::sha256(message.data(), message.size()));
            P.eq("sha256." + v["name"].toString().toStdString(), got == v["sha256"].toString().toStdString() ? 1 : 0, 1);
            ++count;
        }
        P.ge("sha256.answers", count, 4);

        pg::verify::Sha256 made{};
        for (std::size_t i = 0; i < made.size(); ++i)
            made[i] = static_cast<uint8_t>(i * 8 + 7);   // 07 0f 17 ... ff: every digit, the letters among them
        const std::string text = pg::verify::hex(made);
        P.eq("hex.length", static_cast<int64_t>(text.size()), 64);
        P.eq("hex.digits", text == "070f171f272f373f474f575f676f777f878f979fa7afb7bfc7cfd7dfe7eff7ff" ? 1 : 0, 1);
    }

    // ---- KEYS ---------------------------------------------------------------------------------------------------------
    {
        bool f1 = false;
        for (const pg::factory::PublicKey& key : pg::factory::kPublicKeys)
        {
            const std::string id(key.id);
            Bytes fromText;
            const bool decoded = fromBase64(juce::String(std::string(key.base64)), fromText);
            P.eq("keys." + id + ".bytes", static_cast<int64_t>(key.point.size()), 65);
            P.eq("keys." + id + ".prefix_04", key.point[0], 4);
            P.eq("keys." + id + ".base64_same", decoded && fromText == Bytes(key.point.begin(), key.point.end()) ? 1 : 0, 1);
            P.eq("keys." + id + ".imports", pg::verify::p256Point(key.point) ? 1 : 0, 1);
            P.eq("keys." + id + ".found_by_id", pg::factory::publicKey(key.id).data() == key.point.data() ? 1 : 0, 1);
            f1 = f1 || id == "f1";
        }
        P.eq("keys.f1_there", f1 ? 1 : 0, 1);
    }

    // ---- FIXTURES ------------------------------------------------------------------------------------------------------
    {
        juce::NamedValueSet testKeys;
        if (const juce::DynamicObject* o = keysJson.getDynamicObject())
            testKeys = o->getProperties();
        P.eq("fixtures.keys", testKeys.size(), 2);
        for (const auto& k : testKeys)
        {
            const std::string id = k.name.toString().toStdString();
            P.eq("fixtures.key." + id + ".imports", pg::verify::p256Point(bytesOf(k.value)) ? 1 : 0, 1);
            P.eq("fixtures.key." + id + ".not_built_in", pg::factory::publicKey(id).empty() ? 1 : 0, 1);
        }

        std::set<std::string> seen;
        int indexes = 0;
        const juce::Array<juce::var>* list = cases.getArray();
        for (const juce::var& row : list != nullptr ? *list : juce::Array<juce::var>())
        {
            const std::string name = row["case"].toString().toStdString(), key = row["key"].toString().toStdString();
            if (!seen.insert(name + "." + key).second)
                continue;                                // the same set again, after another sequence
            const juce::File dir = fixtures().getChildFile(name);
            const juce::var outer = juce::JSON::parse(dir.getChildFile("factory/v1/" + key + "/index.json").loadFileAsString());
            const juce::var point = testKeys[juce::Identifier(outer["key"].toString().isEmpty() ? "none" : outer["key"].toString())];
            const bool verifies = !point.isVoid()
                               && pg::verify::p256(bytesOf(point), bytesOf(outer["body"].toString().toStdString()), bytesOf(outer["signature"]));
            P.eq("index." + name + "." + key, verifies ? 1 : 0, row["step"].toString() == "signature" ? 0 : 1);
            ++indexes;

            int wrong = 0;
            for (const juce::File& f : dir.getChildFile("projects/v1/p").findChildFiles(juce::File::findFiles, false, "*.fmpack"))
            {
                juce::MemoryBlock data;
                f.loadFileAsData(data);
                const std::string got = pg::verify::hex(pg::verify::sha256(data.getData(), data.getSize()));
                wrong += got == f.getFileNameWithoutExtension().toStdString() ? 0 : 1;
            }
            P.eq("files." + name + "." + key, wrong, name == "file-hash" || name == "file-size" ? 1 : 0);
        }
        P.ge("fixtures.indexes", indexes, 25);
    }
    return P.finish();
}
