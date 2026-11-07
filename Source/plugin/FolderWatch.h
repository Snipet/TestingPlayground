// Source/plugin/FolderWatch.h: the system's word that a folder changed, for FolderSync (JUCE-free). On macOS an
// FSEvents stream over the folder (file-level events, 50 ms latency) on a serial dispatch queue of its own, whose
// callback only sets a flag; elsewhere none (start() is false), and FolderSync looks at the folder every
// FolderSync::kPollSeconds instead. FolderSync looks at the folder now and then even while watched: an event is a
// hint to look sooner, never the only way it learns of a change.
#pragma once

#include <atomic>
#include <string>

namespace playground
{
    class FolderWatch
    {
    public:
        FolderWatch() = default;
        ~FolderWatch();
        FolderWatch(const FolderWatch&) = delete;
        FolderWatch& operator=(const FolderWatch&) = delete;

        // Watch `path` (UTF-8): false when the platform has no watcher or the stream would not start.
        bool start(const std::string& path);
        // Stop: no callback runs after it returns.
        void stop();
        bool watching() const noexcept { return stream_ != nullptr; }
        // Whether something changed since the last call (any thread may set it; the caller's thread clears it).
        bool takeChanged() noexcept { return changed_.exchange(false); }

    private:
        std::atomic<bool> changed_{ false };
        void* stream_ = nullptr;                      // FSEventStreamRef
#if defined(__APPLE__)
        void* queue_ = nullptr;                       // dispatch_queue_t
#endif
    };
}
