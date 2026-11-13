// Source/plugin/ProcessorPreviews.cpp: the Projects card's previews (EditorPort::previewPolled; package 04, editor/
// Preview.h). A version's content comes from the store's version table (versionPolled: never a project's file read
// whole on the message thread; one the table lacks is read on the store's Reader, "reading" until then); its dials,
// its .gui and its shaders are made ready on previewThread_ (preview::prepare: shaderc runs there, never on the message
// thread); poll() draws one at a time on the message thread (bgfx is the message thread's), and none in a poll that
// drew a picture for Claude (view_panel comes first). The newest kPreviewsKept pictures are kept, by the project, the
// version's number, the theme and a hash of the version's files and dials (a version saved again, or a factory set
// replaced, draws again), and let go kPictureHoldSeconds after the card last asked.
#include "plugin/Processor.h"

#include "plugin/ChatImages.h"

#include "editor/Preview.h"
#include "fminus/Failure.h"

#include "Product.h"

#include <algorithm>
#include <chrono>
#include <exception>

namespace playground
{
    namespace
    {
        uint64_t mixIn(uint64_t h, const std::string& s)
        {
            for (const char c : s)
                h = (h ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
            return (h ^ 0xFFu) * 0x100000001B3ull;           // a separator, so that "ab" + "c" is not "a" + "bc"
        }

        uint64_t contentHash(const VersionContent& v)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const SourceFile& f : v.files)
                h = mixIn(mixIn(h, f.name), f.text);
            for (const ContentDial& d : v.dials)
                h = mixIn(mixIn(mixIn(h, d.name), std::to_string(d.value)), d.label);
            return h;
        }

        double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
    }

    PreviewState Processor::previewPolled(const std::string& project, int number, int theme)
    {
        PreviewState out;
        previewAskedAt_ = nowMs();
        if (project.empty())
            return out;
        const VersionContent v = versionPolled(project, number);
        out.number = v.number;
        if (!v.read)
        {
            out.status = PreviewState::Status::reading;
            return out;
        }
        if (v.missing)
        {
            out.status = PreviewState::Status::failed;
            out.why = "there is no such version";
            return out;
        }
        const uint64_t content = contentHash(v);
        const std::lock_guard<std::mutex> lock(previewMutex_);
        for (PreviewEntry& e : previews_)
            if (e.project == project && e.state.number == v.number && e.theme == theme && e.content == content)
            {
                e.used = ++previewUse_;
                return e.state;
            }
        // A new one: prepared on previewThread_, drawn by poll(). The oldest past kPreviewsKept goes (never one that
        // waits for the thread, which holds no picture yet).
        PreviewEntry e;
        e.project = project;
        e.theme = theme;
        e.content = content;
        e.state.status = PreviewState::Status::drawing;
        e.state.number = v.number;
        e.used = ++previewUse_;
        e.queued = true;
        auto request = std::make_shared<preview::Request>();
        request->files = v.files;
        for (const ContentDial& d : v.dials)
            request->dials.push_back({ d.name, d.value, d.label });
        request->instrument = product::kIsInstrument;
        e.request = std::move(request);
        previews_.push_back(std::move(e));
        while (previews_.size() > kPreviewsKept)
        {
            auto oldest = previews_.end();
            for (auto it = previews_.begin(); it != previews_.end(); ++it)
                if (!it->queued && (oldest == previews_.end() || it->used < oldest->used))
                    oldest = it;
            if (oldest == previews_.end())
                break;
            previews_.erase(oldest);
        }
        if (!previewThread_.joinable())
        {
            previewQuit_ = false;
            previewThread_ = std::thread([this] { previewLoop(); });
        }
        previewWake_.notify_all();
        return previews_.back().state;
    }

    void Processor::previewLoop()
    {
        std::unique_lock<std::mutex> lock(previewMutex_);
        for (;;)
        {
            previewWake_.wait(lock, [this] {
                return previewQuit_ || std::any_of(previews_.begin(), previews_.end(), [](const PreviewEntry& e) { return e.queued; });
            });
            if (previewQuit_)
                return;
            // The newest asked first: the card shows the one chosen last.
            auto job = previews_.end();
            for (auto it = previews_.begin(); it != previews_.end(); ++it)
                if (it->queued && (job == previews_.end() || it->used > job->used))
                    job = it;
            // An exception (of any kind: fminus/Failure.h) costs this preview, never the process (this thread's): it
            // fails in words, or, should even those fail (out of memory), with none, never left waiting; the next is
            // answered as any.
            std::shared_ptr<const preview::Request> request;
            std::string project;
            int number = 0, theme = 0;
            uint64_t content = 0;
            std::shared_ptr<const preview::Prepared> prepared;
            try
            {
                request = job->request;
                project = job->project;
                number = job->state.number;
                theme = job->theme;
                content = job->content;
                job->queued = false;
                lock.unlock();
                try
                {
                    failure::at("preview");
                    prepared = std::make_shared<const preview::Prepared>(preview::prepare(*request, previewTools_));
                }
                catch (...)
                {
                    try
                    {
                        auto failed = std::make_shared<preview::Prepared>();
                        failed->why = failure::sentence("preparing it");
                        prepared = std::move(failed);
                    }
                    catch (...)
                    {
                        prepared.reset();                  // failed with no words (below)
                    }
                }
                lock.lock();
            }
            catch (...)
            {
                // Copying the job failed (out of memory), the lock held throughout: that entry fails with no words.
                job->queued = false;
                job->request.reset();
                job->state.status = PreviewState::Status::failed;
                continue;
            }
            for (PreviewEntry& e : previews_)
                if (e.project == project && e.state.number == number && e.theme == theme && e.content == content)
                {
                    e.request.reset();
                    if (!prepared || !prepared->ok)
                    {
                        e.state.status = PreviewState::Status::failed;
                        try
                        {
                            e.state.why = prepared ? prepared->why : std::string();
                        }
                        catch (...)
                        {
                            e.state.why.clear();
                        }
                    }
                    else
                        e.prepared = prepared;
                }
        }
    }

    void Processor::pollPreviews(bool pictureDrawn)
    {
        std::shared_ptr<const preview::Prepared> prepared;
        int theme = 0;
        {
            const std::lock_guard<std::mutex> lock(previewMutex_);
            if (previews_.empty())
                return;
            // The hold: the pictures let go once the card has not asked for one in a while (closed, or gone).
            if (nowMs() - previewAskedAt_ > kPictureHoldSeconds * 1000.0)
            {
                previews_.erase(std::remove_if(previews_.begin(), previews_.end(), [](const PreviewEntry& e) { return !e.queued; }),
                                previews_.end());
                return;
            }
            if (pictureDrawn)
                return;
            // One drawn a poll: the newest asked for that is ready to draw.
            const PreviewEntry* job = nullptr;
            for (const PreviewEntry& e : previews_)
                if (e.prepared != nullptr && (job == nullptr || e.used > job->used))
                    job = &e;
            if (job == nullptr)
                return;
            prepared = job->prepared;
            theme = job->theme;
        }
        const preview::Picture picture = preview::draw(*prepared, theme, preview::kMaxWidth);
        ++previewsDrawn_;
        const std::lock_guard<std::mutex> lock(previewMutex_);
        for (PreviewEntry& e : previews_)
            if (e.prepared == prepared)
            {
                e.prepared.reset();
                if (picture.image.w <= 0 || picture.image.h <= 0)
                {
                    e.state.status = PreviewState::Status::failed;
                    e.state.why = picture.why;
                    continue;
                }
                e.state.status = PreviewState::Status::ready;
                e.state.image = chatimages::nextId();
                e.state.w = picture.image.w;
                e.state.h = picture.image.h;
                e.state.rgba = std::make_shared<const std::vector<uint8_t>>(picture.image.rgba);
                e.state.gpu = picture.gpu;
            }
    }

    void Processor::finishPreviews()
    {
        {
            const std::lock_guard<std::mutex> lock(previewMutex_);
            previewQuit_ = true;
        }
        previewWake_.notify_all();
        if (previewThread_.joinable())
            previewThread_.join();
    }

    std::size_t Processor::previewsKept() const
    {
        const std::lock_guard<std::mutex> lock(previewMutex_);
        return previews_.size();
    }

    bool Processor::previewsWaiting() const
    {
        const std::lock_guard<std::mutex> lock(previewMutex_);
        return std::any_of(previews_.begin(), previews_.end(), [](const PreviewEntry& e) { return e.queued || e.prepared != nullptr; });
    }

    std::size_t Processor::previewsQueued() const
    {
        const std::lock_guard<std::mutex> lock(previewMutex_);
        return static_cast<std::size_t>(std::count_if(previews_.begin(), previews_.end(), [](const PreviewEntry& e) { return e.queued; }));
    }

    std::size_t Processor::previewsToDraw() const
    {
        const std::lock_guard<std::mutex> lock(previewMutex_);
        return static_cast<std::size_t>(std::count_if(previews_.begin(), previews_.end(), [](const PreviewEntry& e) { return e.prepared != nullptr; }));
    }

    bool Processor::pictureWaiting() const
    {
        const std::lock_guard<std::mutex> lock(pictureMutex_);
        return pictureAsked_;
    }

    bool Processor::previewsSettle(double seconds)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (previewsWaiting())
        {
            if (std::chrono::steady_clock::now() >= until)
                return false;
            poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
}
