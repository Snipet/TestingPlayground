// Source/plugin/ProcessorAssets.cpp: the Processor's samples (Processor.h; dsp/Assets.h): the set a program's #pragma
// sample lines read, the explorer's ASSETS (EditorPort::assets) and Claude's (ChatHost::chatAssets, chatAddAsset, ...),
// what the session and a project version keep of them. Every file is decoded where it is added (AssetFiles.h): the
// message thread for a drop or the explorer's Add a sample..., Claude's worker for write_file's path, the thread that
// loads a state or opens a version; never the audio thread, which only reads the tables a Patch holds.
#include "plugin/Processor.h"

#include <algorithm>
#include <map>

namespace playground
{
    namespace
    {
        const juce::Identifier kAssetsTag{ "ASSETS" }, kAssetTag{ "ASSET" }, kAssetName{ "name" }, kAssetData{ "data" };

        std::string joined(const std::vector<std::string>& v)
        {
            std::string out;
            for (const std::string& s : v)
                out += (out.empty() ? "" : "; ") + s;
            return out;
        }
    }

    Assets Processor::assetsNow() const
    {
        const std::lock_guard<std::mutex> lock(assetsMutex_);
        return assets_;
    }

    void Processor::replaceAssets(Assets next, Journal::Who who, const std::string& words)
    {
        {
            const std::lock_guard<std::mutex> lock(assetsMutex_);
            assets_ = std::move(next);
            compiler_.setAssets(assets_);                    // under the lock: two replacements reach it in order
        }
        ++assetsRevision_;
        if (!words.empty())
            journal_.note(who, words);
    }

    bool Processor::putAsset(const std::shared_ptr<const assets::Asset>& asset, Journal::Who who, std::string& why)
    {
        bool replaced = false;
        {
            const std::lock_guard<std::mutex> lock(assetsMutex_);
            // The caps again, against the set as it is now (another sample may have come while this one decoded).
            if (const std::string refused = assets::refusal(assets_.get(), asset->name(), asset->bytes().size()); !refused.empty())
            {
                why = refused;
                return false;
            }
            replaced = assets_ != nullptr && assets_->find(asset->name()) != nullptr;
            assets_ = assets::with(assets_, asset);
            compiler_.setAssets(assets_);
        }
        ++assetsRevision_;
        journal_.note(who, std::string(replaced ? "replaced" : "added") + " the sample " + asset->name() + " ("
                               + asset->describe() + ")");
        return true;
    }

    bool Processor::dropAsset(const std::string& name, Journal::Who who, std::string& why)
    {
        Assets now = assetsNow();
        if (now == nullptr || now->find(name) == nullptr)
        {
            why = "the program's samples have no " + name;
            return false;
        }
        replaceAssets(assets::without(now, name), who, "removed the sample " + name);
        return true;
    }

    // ---- EditorPort (message thread) --------------------------------------------------------------------------------
    std::vector<AssetEntry> Processor::assets() const
    {
        std::vector<AssetEntry> out;
        const Assets now = assetsNow();
        if (now == nullptr)
            return out;
        const CompileStatus s = compiler_.status();
        for (const std::shared_ptr<const assets::Asset>& a : now->items)
        {
            AssetEntry e;
            e.name = a->name();
            e.detail = a->describe();
            e.used = std::any_of(s.samples.begin(), s.samples.end(),
                                 [&](const auto& u) { return u.first == a->name() && u.second == a->hash(); });
            out.push_back(std::move(e));
        }
        return out;
    }

    int Processor::addAssets(const std::vector<std::string>& paths, std::string& why)
    {
        std::vector<std::string> refused;
        int added = 0;
        for (const std::string& path : paths)
        {
            std::string w;
            const std::shared_ptr<const assets::Asset> a = assetfiles::read(assetsNow().get(), path, "", w);
            if (a != nullptr && putAsset(a, Journal::Who::user, w))
                ++added;
            else
                refused.push_back(w);
        }
        why = joined(refused);
        return added;
    }

    bool Processor::removeAsset(const std::string& name, std::string& why)
    {
        return dropAsset(name, Journal::Who::user, why);
    }

    // ---- ChatHost (Claude's worker) ---------------------------------------------------------------------------------
    bool Processor::chatAddAsset(const std::string& path, const std::string& name, std::string& done, std::string& why)
    {
        const std::shared_ptr<const assets::Asset> a = assetfiles::read(assetsNow().get(), path, name, why);
        if (a == nullptr)
            return false;
        const Assets before = assetsNow();
        const bool replaces = before != nullptr && before->find(a->name()) != nullptr;
        if (!putAsset(a, Journal::Who::chat, why))
            return false;
        done = std::string(replaces ? "Replaced" : "Added") + " the sample " + a->name() + " (" + a->describe() + ")";
        return true;
    }

    bool Processor::chatRemoveAsset(const std::string& name, std::string& why)
    {
        return dropAsset(name, Journal::Who::chat, why);
    }

    bool Processor::chatRenameAsset(const std::string& name, const std::string& to, std::string& why)
    {
        const Assets now = assetsNow();
        if (now == nullptr || now->find(name) == nullptr)
        {
            why = "the program's samples have no " + name;
            return false;
        }
        if (!assets::validName(to))
        {
            why = to + ": a sample's name is a valid file name ending in .wav, .aif, .aiff or .flac";
            return false;
        }
        if (to != name && now->find(to) != nullptr)
        {
            why = "a sample is named " + to + " already";
            return false;
        }
        replaceAssets(assets::renamed(now, name, to), Journal::Who::chat, "renamed the sample " + name + " to " + to);
        return true;
    }

    // ---- the session and the projects -------------------------------------------------------------------------------
    juce::ValueTree Processor::assetsState() const
    {
        juce::ValueTree tree{ kAssetsTag };
        const std::lock_guard<std::mutex> lock(assetsMutex_);
        if (assets_ == nullptr)
            return tree;
        std::map<std::string, std::string> kept;          // each file encoded once, while the set holds it
        for (const std::shared_ptr<const assets::Asset>& a : assets_->items)
        {
            auto it = base64_.find(a->hash());
            std::string text = it != base64_.end() ? it->second : assetfiles::toBase64(a->bytes());
            juce::ValueTree t{ kAssetTag };
            t.setProperty(kAssetName, juce::String::fromUTF8(a->name().c_str()), nullptr);
            t.setProperty(kAssetData, juce::String(text), nullptr);
            tree.appendChild(t, nullptr);
            kept[a->hash()] = std::move(text);
        }
        base64_ = std::move(kept);
        return tree;
    }

    // Each file in by its name: a second of a name (a damaged or hand-edited state) takes the first's place, as
    // make()'s caps counted it (assets::with), so a set never holds two of one name.
    Assets Processor::assetsOfState(const juce::ValueTree& state, std::vector<std::string>& refused)
    {
        Assets set = std::make_shared<const assets::Set>();
        const juce::ValueTree tree = state.getChildWithName(kAssetsTag);
        for (int k = 0; tree.isValid() && k < tree.getNumChildren(); ++k)
            if (const juce::ValueTree t = tree.getChild(k); t.hasType(kAssetTag))
            {
                const std::string name = t.getProperty(kAssetName).toString().substring(0, 64).toStdString();
                const std::optional<std::string> bytes = assetfiles::fromBase64(t.getProperty(kAssetData).toString().toStdString());
                std::string why;
                if (!bytes)
                    refused.push_back(name + " is damaged in the saved state");
                else if (std::shared_ptr<const assets::Asset> a = assetfiles::make(set.get(), name, *bytes, why))
                    set = assets::with(set, std::move(a));
                else
                    refused.push_back(why);
            }
        return set;
    }

    Assets Processor::assetsOfVersion(const projectstore::Project& p, const projectstore::Version& v,
                                      const Assets& reuse, std::vector<std::string>& refused)
    {
        Assets set = std::make_shared<const assets::Set>();
        for (const projectstore::SampleRef& r : v.samples)
        {
            const projectstore::SampleFile* f = p.sample(r.hash);
            std::shared_ptr<const assets::Asset> same;   // the file `reuse` holds already: its sound, not decoded again
            if (reuse != nullptr && f != nullptr)
                for (const std::shared_ptr<const assets::Asset>& a : reuse->items)
                    if (a->hash() == r.hash && a->bytes() == f->bytes)
                    {
                        same = a->name() == r.name ? a : std::make_shared<const assets::Asset>(r.name, *a);
                        break;
                    }
            std::string why;
            if (same != nullptr && assets::refusal(set.get(), r.name, same->bytes().size()).empty())
                set = assets::with(set, std::move(same));
            else if (f == nullptr)
                refused.push_back("\"" + p.name + "\" keeps no file for its sample " + r.name);
            else if (std::shared_ptr<const assets::Asset> a = assetfiles::make(set.get(), r.name, f->bytes, why))
                set = assets::with(set, std::move(a));
            else
                refused.push_back(why);
        }
        return set;
    }

    Assets Processor::chatVersionSamples(const projectstore::Project& p, const projectstore::Version& v,
                                         std::vector<std::string>& refused) const
    {
        return assetsOfVersion(p, v, assetsNow(), refused);
    }
}
