// Source/fminus/ScriptWatcher.h: live coding. Watches a script's file (Script.h) on a thread of its own, never the
// audio thread, and loads it into the Script whenever it changes: save the file in an editor and, half a second later,
// the plugin plays the new script, faded over from the old one. Each load writes its outcome beside the file, in
// <file>.log: the time, then "loaded" or FMinus's errors (the running script runs on after an error). The thread also
// frees what the audio thread hands back (Script::collect).
#pragma once

#include "fminus/Script.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace testingplaygroundsynth
{
    class ScriptWatcher
    {
    public:
        // Starts watching `path`, a file that need not exist yet. `loaded` is the source the Script runs already (the
        // plugin's own copy): the file is loaded only when it says something else.
        ScriptWatcher(Script& script, std::filesystem::path path, std::string loaded);
        ~ScriptWatcher();                          // stops the thread and waits for it
        ScriptWatcher(const ScriptWatcher&) = delete;
        ScriptWatcher& operator=(const ScriptWatcher&) = delete;

    private:
        void run();
        void check();
        void log(const std::string& text) const;

        Script& script_;
        const std::filesystem::path path_;
        std::string loaded_;                       // the source the Script was last given
        std::filesystem::file_time_type seen_{};   // the file's time when it was last read
        bool read_ = false;                        // the file has been read at least once
        std::mutex mutex_;
        std::condition_variable wake_;
        bool stop_ = false;
        std::thread thread_;
    };
} // namespace testingplaygroundsynth
