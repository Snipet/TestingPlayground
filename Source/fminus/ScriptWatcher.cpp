// Source/fminus/ScriptWatcher.cpp: see ScriptWatcher.h.
#include "fminus/ScriptWatcher.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace testingplaygroundsynth
{
    ScriptWatcher::ScriptWatcher(Script& script, std::filesystem::path path, std::string loaded)
        : script_(script), path_(std::move(path)), loaded_(std::move(loaded)), thread_([this] { run(); })
    {
    }

    ScriptWatcher::~ScriptWatcher()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    void ScriptWatcher::run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_)
        {
            lock.unlock();
            check();
            script_.collect();
            lock.lock();
            wake_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stop_; });
        }
    }

    // A new time on the file: read it, and load it when it differs from what runs.
    void ScriptWatcher::check()
    {
        std::error_code error;
        const std::filesystem::file_time_type time = std::filesystem::last_write_time(path_, error);
        if (error || (read_ && time == seen_))
            return;
        std::ifstream file(path_, std::ios::binary);
        if (!file)
            return;
        std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        seen_ = time;
        read_ = true;
        if (source == loaded_)
            return;
        std::string errors;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = script_.load(source, path_.filename().string(), errors);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ok)
            loaded_ = std::move(source);
        log(ok ? "loaded, compiled in " + std::to_string(static_cast<int>(ms + 0.5)) + " ms\n"
               : "not loaded (the running script runs on):\n" + errors);
    }

    void ScriptWatcher::log(const std::string& text) const
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);                 // localtime is not thread-safe, and each plugin has a watcher
        char stamp[32] = {};
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
        std::ofstream out(path_.string() + ".log", std::ios::trunc);
        out << stamp << " " << path_.filename().string() << ": " << text;
    }
} // namespace testingplaygroundsynth
