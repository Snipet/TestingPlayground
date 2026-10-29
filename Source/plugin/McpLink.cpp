// Source/plugin/McpLink.cpp: see McpLink.h.
#include "plugin/McpLink.h"

#include "mcp/McpProtocol.h"

#include "Product.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace playground
{
    namespace
    {
        // Written beside, then renamed over: another instance or the server may be reading it.
        void writeAtomically(const juce::File& f, const juce::String& text)
        {
            const juce::File temp = f.getSiblingFile("." + f.getFileName() + "." + juce::String(static_cast<int>(getpid())) + ".writing");
            if (temp.replaceWithText(text, false, false, "\n"))
                temp.moveFileTo(f);
        }

        juce::var words(const std::string& t)
        {
            juce::Array<juce::var> a;
            auto* o = new juce::DynamicObject();
            o->setProperty("type", "text");
            o->setProperty("text", juce::String::fromUTF8(t.c_str()));
            a.add(juce::var(o));
            return a;
        }

        juce::var replyTo(const juce::var& id)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("id", id);
            return juce::var(o);
        }
    }

    // A connection from the server: its socket (non-blocking), closed when the last of the poll thread and the jobs
    // lets go of it.
    struct McpLink::Client
    {
        explicit Client(int fd) : reader(fd) {}
        ~Client() { close(reader.fd()); }
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        mcp::LineReader reader;                          // the poll thread's
        bool spoke = false;                              // it has sent a request (it counts as connected)
        std::string out;                                 // replies the socket has not taken yet, whole lines (outMutex_)
    };

    McpLink::McpLink(Hooks hooks) : hooks_(std::move(hooks))
    {
        const std::string dir = mcp::folder();
        if (dir.empty())
            return;
        const juce::File folder(juce::String::fromUTF8(dir.c_str()));
        writeAtomically(folder.getChildFile("tools.json"), juce::JSON::toString(ChatAgent::mcpTools(), false));
        writeAtomically(folder.getChildFile("guide.md"), juce::String::fromUTF8(ChatAgent::mcpGuide().c_str()));
        int number = 0;
        listen_ = mcp::claim(dir, number, path_);
        if (listen_ < 0)
        {
            path_.clear();
            return;
        }
        id_ = std::to_string(number);
        name_ = std::string(product::kIsInstrument ? "Synth " : "Effect ") + id_;
        if (pipe(wake_) != 0 || !mcp::nonBlocking(wake_[0]) || !mcp::nonBlocking(wake_[1]))
        {
            close(listen_);
            listen_ = -1;
            unlink(path_.c_str());
            path_.clear();
            return;
        }
        worker_ = std::thread([this] { work(); });
        thread_ = std::thread([this] { run(); });
    }

    McpLink::~McpLink()
    {
        stop_.store(true);
        wake();
        if (thread_.joinable())
            thread_.join();
        // The listens: cancelled (each stops within a read) and waited for.
        std::list<Listening> listens;
        {
            const std::lock_guard<std::mutex> lock(jobMutex_);
            listens.swap(listens_);
        }
        for (const Listening& l : listens)
            if (hooks_.cancel)
                hooks_.cancel(l.call);
        for (Listening& l : listens)
            if (l.thread.joinable())
                l.thread.join();
        // The worker: what waits is dropped, what runs is cancelled (a tune stops within a render) and waited for.
        uint64_t running = 0;
        {
            const std::lock_guard<std::mutex> lock(jobMutex_);
            workerStop_ = true;
            jobs_.clear();
            running = running_;
        }
        jobCv_.notify_all();
        if (running != 0 && hooks_.cancel)
            hooks_.cancel(running);
        if (worker_.joinable())
            worker_.join();
        if (listen_ >= 0)
        {
            close(listen_);
            unlink(path_.c_str());
        }
        for (const int fd : wake_)
            if (fd >= 0)
                close(fd);
    }

    void McpLink::wake()
    {
        if (wake_[1] < 0)
            return;
        const char x = 1;
        [[maybe_unused]] const ssize_t n = write(wake_[1], &x, 1);   // a pipe full is a poll thread already woken
    }

    void McpLink::run()
    {
        std::vector<std::shared_ptr<Client>> clients;
        while (!stop_.load())
        {
            std::vector<pollfd> fds;
            fds.push_back({ wake_[0], POLLIN, 0 });
            fds.push_back({ listen_, POLLIN, 0 });
            {
                const std::lock_guard<std::mutex> lock(outMutex_);
                for (const auto& c : clients)
                    fds.push_back({ c->reader.fd(), static_cast<short>(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0 });
            }
            const int r = poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
            if (r < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (fds[0].revents != 0)
            {
                char drain[256];
                while (read(wake_[0], drain, sizeof drain) > 0)
                {
                }
            }
            if (stop_.load())
                break;
            {
                const std::lock_guard<std::mutex> lock(jobMutex_);
                reap();
            }
            const std::size_t polled = fds.size() - 2;   // the clients before this round's accept
            std::vector<bool> dead(clients.size(), false);
            for (std::size_t k = 0; k < polled; ++k)
            {
                const short revents = fds[k + 2].revents;
                if (revents == 0)
                    continue;
                const std::shared_ptr<Client>& c = clients[k];
                if ((revents & POLLOUT) != 0)
                {
                    const std::lock_guard<std::mutex> lock(outMutex_);
                    mcp::flush(c->reader.fd(), c->out);  // the other end gone: its read below says so
                }
                if ((revents & ~POLLOUT) == 0)
                    continue;
                if (!c->reader.readSome())
                {
                    dead[k] = true;
                    continue;
                }
                std::string line;
                while (!stop_.load() && c->reader.take(line))
                {
                    if (line.empty())
                        continue;
                    if (!c->spoke)
                    {
                        c->spoke = true;
                        ++talking_;
                    }
                    request(c, line);
                }
            }
            for (std::size_t k = dead.size(); k-- > 0;)
                if (dead[k])
                {
                    if (clients[k]->spoke)
                        --talking_;
                    closed(clients[k]);
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(k));
                }
            if ((fds[1].revents & POLLIN) != 0)
                if (const int fd = accept(listen_, nullptr, nullptr); fd >= 0)
                {
                    mcp::noSigpipe(fd);
                    if (mcp::nonBlocking(fd))
                        clients.push_back(std::make_shared<Client>(fd));
                    else
                        close(fd);
                }
        }
        for (const auto& c : clients)
            closed(c);
    }

    // One request: answered here, or (a call that is not live) queued for the worker.
    void McpLink::request(const std::shared_ptr<Client>& client, const std::string& line)
    {
        const juce::var request = juce::JSON::parse(juce::String::fromUTF8(line.c_str()));
        const juce::var id = request.getProperty("id", juce::var());
        const juce::String method = request["method"].toString();
        const juce::var params = request["params"];
        juce::var reply = replyTo(id);
        juce::DynamicObject& o = *reply.getDynamicObject();
        if (method == "info")
        {
            juce::var info = hooks_.info ? hooks_.info() : juce::var(new juce::DynamicObject());
            if (juce::DynamicObject* i = info.getDynamicObject())
            {
                i->setProperty("id", juce::String(id_));
                i->setProperty("name", juce::String(name_));
            }
            o.setProperty("result", info);
        }
        else if (method == "tools")
            o.setProperty("result", ChatAgent::mcpTools());
        else if (method == "guide")
        {
            // No part named: a 0.2.0 server, which knows no parts and has no way to ask for the others (where the tools'
            // short descriptions send Claude), so all of it.
            const juce::DynamicObject* p = params.getDynamicObject();
            const std::string part = p != nullptr && p->hasProperty("part") ? params["part"].toString().toStdString() : "all";
            bool found = true;
            const std::string text = ChatAgent::mcpGuidePart(part, found);
            o.setProperty(found ? "result" : "error", juce::String::fromUTF8(text.c_str()));
        }
        else if (method == "cancel")
        {
            bool found = false;
            cancel(client, params["id"], found);
            auto* res = new juce::DynamicObject();
            res->setProperty("cancelled", found);
            o.setProperty("result", juce::var(res));
        }
        else if (method == "call" && hooks_.call)
        {
            const std::string name = params["name"].toString().toStdString();
            const juce::var arguments = params["arguments"];
            if (!ChatAgent::mcpLive(name, arguments))
            {
                {
                    const std::lock_guard<std::mutex> lock(jobMutex_);
                    jobs_.push_back({ client, id, nextCall_++, name, arguments });
                }
                jobCv_.notify_all();
                return;                                  // the worker answers
            }
            if (ChatAgent::mcpLiveWaits(name))
            {
                if (listen(client, id, name, arguments))
                    return;                              // its thread answers
                ChatAgent::McpResult r;
                r.content = words("The plugin is listening " + std::to_string(kListens) + " times already: wait for one to end, "
                                  "then call again.");
                r.isError = true;
                o.setProperty("result", resultOf(r));
            }
            else
                o.setProperty("result", resultOf(hooks_.call(name, arguments, 0)));
        }
        else
            o.setProperty("error", "there is no method \"" + method + "\"");
        answer(*client, reply);
    }

    // Stop request `id`'s call: dropped when it waits (its reply says so), cancelled when it runs.
    void McpLink::cancel(const std::shared_ptr<Client>& client, const juce::var& id, bool& found)
    {
        uint64_t running = 0;
        std::optional<Job> dropped;
        {
            const std::lock_guard<std::mutex> lock(jobMutex_);
            for (const Listening& l : listens_)
                if (l.client == client && l.id == id && !l.done->load())
                    running = l.call;                    // a listen: it stops and replies with what it heard
            if (running == 0 && running_ != 0 && runningClient_ == client && runningId_ == id)
                running = running_;
            else if (running == 0)
                for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
                    if (it->client == client && it->id == id)
                    {
                        dropped = std::move(*it);
                        jobs_.erase(it);
                        break;
                    }
        }
        found = running != 0 || dropped.has_value();
        if (running != 0 && hooks_.cancel)
            hooks_.cancel(running);
        if (dropped)
        {
            ChatAgent::McpResult r;
            r.content = words("Stopped before it started.");
            r.isError = true;
            juce::var reply = replyTo(dropped->id);
            reply.getDynamicObject()->setProperty("result", resultOf(r));
            answer(*client, reply);
        }
    }

    // A connection gone: nobody waits for its calls.
    void McpLink::closed(const std::shared_ptr<Client>& client)
    {
        uint64_t running = 0;
        std::vector<uint64_t> listening;
        {
            const std::lock_guard<std::mutex> lock(jobMutex_);
            jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.client == client; }), jobs_.end());
            if (running_ != 0 && runningClient_ == client)
                running = running_;
            for (const Listening& l : listens_)
                if (l.client == client)
                    listening.push_back(l.call);
        }
        if (running != 0 && hooks_.cancel)
            hooks_.cancel(running);
        for (const uint64_t call : listening)
            if (hooks_.cancel)
                hooks_.cancel(call);
    }

    bool McpLink::listen(const std::shared_ptr<Client>& client, const juce::var& id, const std::string& name, const juce::var& arguments)
    {
        const std::lock_guard<std::mutex> lock(jobMutex_);
        reap();
        if (workerStop_ || stop_.load() || listens_.size() >= kListens)
            return false;
        Listening& l = listens_.emplace_back();
        l.client = client;
        l.id = id;
        l.call = nextCall_++;
        l.done = std::make_shared<std::atomic<bool>>(false);
        l.thread = std::thread([this, client, id, name, arguments, call = l.call, done = l.done] {
            const ChatAgent::McpResult r = hooks_.call(name, arguments, call);
            juce::var reply = replyTo(id);
            reply.getDynamicObject()->setProperty("result", resultOf(r));
            answer(*client, reply);                      // to a connection gone: nothing is read, nothing breaks
            done->store(true);
            wake();                                      // the poll thread joins it
        });
        return true;
    }

    void McpLink::reap()
    {
        for (auto it = listens_.begin(); it != listens_.end();)
            if (it->done->load())
            {
                if (it->thread.joinable())
                    it->thread.join();
                it = listens_.erase(it);
            }
            else
                ++it;
    }

    void McpLink::work()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                jobCv_.wait(lock, [this] { return workerStop_ || !jobs_.empty(); });
                if (workerStop_)
                    return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
                running_ = job.call;
                runningClient_ = job.client;
                runningId_ = job.id;
            }
            const ChatAgent::McpResult r = hooks_.call(job.name, job.arguments, job.call);
            {
                const std::lock_guard<std::mutex> lock(jobMutex_);
                running_ = 0;
                runningClient_.reset();
                runningId_ = juce::var();
            }
            juce::var reply = replyTo(job.id);
            reply.getDynamicObject()->setProperty("result", resultOf(r));
            answer(*job.client, reply);                   // to a connection gone: nothing is read, nothing breaks
        }
    }

    void McpLink::answer(Client& client, const juce::var& reply)
    {
        const std::string line = juce::JSON::toString(reply, true).toStdString() + "\n";
        bool left = false;
        {
            const std::lock_guard<std::mutex> lock(outMutex_);
            client.out += line;
            mcp::flush(client.reader.fd(), client.out);
            left = !client.out.empty();
        }
        if (left)
            wake();                                      // the poll thread sends the rest when the socket has room
    }

    juce::var McpLink::resultOf(const ChatAgent::McpResult& r)
    {
        auto* res = new juce::DynamicObject();
        res->setProperty("content", r.content);
        res->setProperty("isError", r.isError);
        return juce::var(res);
    }
}
