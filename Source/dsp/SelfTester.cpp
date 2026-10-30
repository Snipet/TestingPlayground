// Source/dsp/SelfTester.cpp: see SelfTester.h.
#include "dsp/SelfTester.h"

#include "fminus/Circuit.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <utility>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace playground
{
    namespace
    {
        // The calling thread below the host's audio and its UI: a test is background work.
        void utilityQos()
        {
#if defined(__APPLE__)
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
        }

        // A test's workers: `count` threads that run SelfTestOptions::parallel's batches beside the thread that asks,
        // over an atomic counter, made for one test and joined when it ends (the destructor).
        class Workers
        {
        public:
            explicit Workers(int count)
            {
                for (int k = 0; k < count; ++k)
                    threads_.emplace_back([this] { loop(); });
            }

            ~Workers()
            {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    stop_ = true;
                }
                wake_.notify_all();
                for (std::thread& t : threads_)
                    t.join();
            }

            Workers(const Workers&) = delete;
            Workers& operator=(const Workers&) = delete;

            // job(0) to job(count - 1), each once, here and on the workers; returns when all have returned and no worker
            // holds the batch any more (so a late one never runs a job whose caller has gone).
            void run(int count, const std::function<void(int)>& job)
            {
                if (count <= 0)
                    return;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    job_ = &job;
                    count_ = count;
                    next_.store(0, std::memory_order_relaxed);
                    finished_.store(0, std::memory_order_relaxed);
                    ++batch_;
                }
                wake_.notify_all();
                work(job, count);
                std::unique_lock<std::mutex> lock(mutex_);
                done_.wait(lock, [&] { return finished_.load(std::memory_order_acquire) == count && active_ == 0; });
                job_ = nullptr;
            }

        private:
            void work(const std::function<void(int)>& job, int count)
            {
                for (int i = next_.fetch_add(1, std::memory_order_relaxed); i < count;
                     i = next_.fetch_add(1, std::memory_order_relaxed))
                {
                    job(i);
                    finished_.fetch_add(1, std::memory_order_acq_rel);
                }
            }

            void loop()
            {
                utilityQos();
                uint64_t seen = 0;
                std::unique_lock<std::mutex> lock(mutex_);
                for (;;)
                {
                    wake_.wait(lock, [&] { return stop_ || batch_ != seen; });
                    if (stop_)
                        return;
                    seen = batch_;
                    if (job_ == nullptr)
                        continue;                        // a batch over before this worker woke
                    const std::function<void(int)>& job = *job_;
                    const int count = count_;
                    ++active_;
                    lock.unlock();
                    work(job, count);
                    lock.lock();
                    --active_;
                    done_.notify_all();
                }
            }

            std::mutex mutex_;
            std::condition_variable wake_, done_;
            const std::function<void(int)>* job_ = nullptr;   // the batch running (mutex_), null between batches
            int count_ = 0;
            uint64_t batch_ = 0;
            int active_ = 0;                             // workers inside the batch (mutex_)
            bool stop_ = false;
            std::atomic<int> next_{ 0 }, finished_{ 0 };
            std::vector<std::thread> threads_;           // last: made once the rest is
        };
    }

    int SelfTester::workers()
    {
        const int cores = static_cast<int>(std::thread::hardware_concurrency());
        return std::min(4, std::max(1, cores - 2));
    }

    SelfTester::~SelfTester()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        stopping_.store(true, std::memory_order_relaxed);
        cancel_.store(true, std::memory_order_relaxed);
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    void SelfTester::want(std::vector<circuits::Job> jobs)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!running_.empty()
                && std::none_of(jobs.begin(), jobs.end(), [&](const circuits::Job& j) { return j.key == running_; }))
                cancel_.store(true, std::memory_order_relaxed);   // the newest program does not want it
            wanted_ = std::move(jobs);
            changed_ = true;
            if (!thread_.joinable() && !wanted_.empty() && !stop_)
                thread_ = std::thread([this] { coordinator(); });
        }
        wake_.notify_all();
    }

    void SelfTester::setEnabled(bool on)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            enabled_ = on;
            changed_ = true;
            if (!on && !running_.empty())
                cancel_.store(true, std::memory_order_relaxed);
        }
        wake_.notify_all();
    }

    bool SelfTester::busy() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.empty())
            return true;
        for (const circuits::Job& j : wanted_)
            if (!circuits::answers().gaveUp(j.key) && !circuits::answers().find(j.key))
                return true;
        return false;
    }

    bool SelfTester::next(circuits::Job& job, bool any)
    {
        if (stop_ || (!any && !enabled_) || !running_.empty())
            return false;
        for (const circuits::Job& j : wanted_)
            if (circuits::answers().claim(j.key))   // neither stored, given up on, nor in flight
            {
                job = j;
                running_ = j.key;
                cancel_.store(false, std::memory_order_relaxed);
                return true;
            }
        return false;
    }

    void SelfTester::test(const circuits::Job& job)
    {
        tests_.fetch_add(1, std::memory_order_relaxed);
        fminus::circuit::SelfTestOptions o;
        bool answered = false, cancelled = false;
        {
            Workers workers(SelfTester::workers());
            o.parallel = [&workers](int count, const std::function<void(int)>& run) { workers.run(count, run); };
            o.cancelled = [this] {
                return cancel_.load(std::memory_order_relaxed) || stopping_.load(std::memory_order_relaxed);
            };
            o.budget = budget_.load(std::memory_order_relaxed);
            const fminus::circuit::SelfTestResult r = fminus::circuit::selfTest(job.netlist, job.file, {}, o);
            // Cancelled once it was over is cancelled too: a key the newest program dropped stores nothing.
            cancelled = r.cancelled || cancel_.load(std::memory_order_relaxed) || stopping_.load(std::memory_order_relaxed);
            answered = r.choice.has_value() && !cancelled && r.key == job.key;
            // What it said beyond the answer, before the answer (a compiler that sees landed() move reads it): its own
            // warnings, and why when it gave none (errors, over the budget, nothing finite under the cost cap): such a
            // key is not tested again, and its compiles say why.
            if (!cancelled)
                circuits::answers().report(job.key, circuits::outcomeOf(job, answered, r.warnings, r.notes, r.errors));
            if (answered)
                circuits::answers().store(job.key, r.choice->text());
        }
        circuits::answers().release(job.key);       // after the store: the key is never neither stored nor in flight
        const std::lock_guard<std::mutex> lock(mutex_);
        running_.clear();
    }

    void SelfTester::runNow()
    {
        const std::lock_guard<std::mutex> one(testMutex_);
        for (;;)
        {
            circuits::Job job;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (!next(job, true))
                    return;
            }
            test(job);
        }
    }

    void SelfTester::coordinator()
    {
        utilityQos();
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_)
        {
            // Woken by a change; while jobs are wanted, every tenth of a second too (another tester's key released, or
            // stored).
            if (wanted_.empty())
                wake_.wait(lock, [this] { return stop_ || changed_; });
            else
                wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stop_ || changed_; });
            changed_ = false;
            if (stop_)
                break;
            lock.unlock();
            {
                const std::lock_guard<std::mutex> one(testMutex_);
                circuits::Job job;
                bool picked = false;
                {
                    const std::lock_guard<std::mutex> again(mutex_);
                    picked = next(job, false);
                }
                if (picked)
                    test(job);
            }
            lock.lock();
        }
    }
}
