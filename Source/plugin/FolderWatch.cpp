// Source/plugin/FolderWatch.cpp: see FolderWatch.h.
#include "plugin/FolderWatch.h"

#if defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace playground
{
    FolderWatch::~FolderWatch() { stop(); }

#if defined(__APPLE__)
    namespace
    {
        void onEvents(ConstFSEventStreamRef, void* info, size_t count, void*, const FSEventStreamEventFlags*,
                      const FSEventStreamEventId*)
        {
            if (count > 0)
                static_cast<std::atomic<bool>*>(info)->store(true);
        }
    }

    bool FolderWatch::start(const std::string& path)
    {
        stop();
        CFStringRef p = CFStringCreateWithCString(kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
        if (p == nullptr)
            return false;
        const void* one[] = { p };
        CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault, one, 1, &kCFTypeArrayCallBacks);
        CFRelease(p);
        if (paths == nullptr)
            return false;
        FSEventStreamContext context{};
        context.info = &changed_;
        FSEventStreamRef stream = FSEventStreamCreate(kCFAllocatorDefault, &onEvents, &context, paths,
                                                      kFSEventStreamEventIdSinceNow, 0.05,
                                                      kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
        CFRelease(paths);
        if (stream == nullptr)
            return false;
        dispatch_queue_t queue = dispatch_queue_create("folder-watch", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(stream, queue);
        if (!FSEventStreamStart(stream))
        {
            FSEventStreamInvalidate(stream);
            FSEventStreamRelease(stream);
            dispatch_release(queue);
            return false;
        }
        stream_ = stream;
        queue_ = queue;
        return true;
    }

    void FolderWatch::stop()
    {
        if (stream_ == nullptr)
            return;
        auto* stream = static_cast<FSEventStreamRef>(stream_);
        auto queue = static_cast<dispatch_queue_t>(queue_);
        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        dispatch_sync_f(queue, nullptr, [](void*) {});   // a callback in flight finishes before the flag can go
        FSEventStreamRelease(stream);
        dispatch_release(queue);
        stream_ = nullptr;
        queue_ = nullptr;
    }
#else
    bool FolderWatch::start(const std::string& path)
    {
        (void)path;
        return false;                                    // FolderSync looks every kPollSeconds
    }

    void FolderWatch::stop() {}
#endif
}
