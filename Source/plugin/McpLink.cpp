// Source/plugin/McpLink.cpp: see McpLink.h.
#include "plugin/McpLink.h"

#include "mcp/McpProtocol.h"
#include "mcp/McpTarget.h"

#include "Product.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace playground
{
    namespace
    {
        using mcp::target::writeAtomically;

        constexpr auto kScanEvery = std::chrono::milliseconds(1000);   // the folder read again (the others, the choice)

        // A card's token: random, its own (an instance that takes the number of one gone is never taken for it).
        std::string newToken()
        {
            std::random_device rd;
            const uint64_t a = (static_cast<uint64_t>(rd()) << 32) ^ rd();
            const uint64_t b = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            return juce::String::toHexString(static_cast<juce::int64>(a ^ (b * 0x9E3779B97F4A7C15ull))).paddedLeft('0', 16).toStdString();
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
        ~Client() { mcp::closeFd(reader.fd()); }
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
        dir_ = dir;
        token_ = newToken();
        if (!mcp::socketPair(wake_) || !mcp::nonBlocking(wake_[0]) || !mcp::nonBlocking(wake_[1]))
        {
            mcp::closeFd(listen_);
            listen_ = -1;
            mcp::removePath(path_);
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
            mcp::closeFd(listen_);
            mcp::removePath(path_);
            mcp::target::removeCard(dir_, id_, token_);
            mcp::target::clearTarget(dir_, token_);    // the user's choice goes with it
        }
        for (const int fd : wake_)
            if (fd >= 0)
                mcp::closeFd(fd);
    }

    std::string McpLink::name() const
    {
        if (id_.empty())
            return {};
        const std::lock_guard<std::mutex> lock(nameMutex_);
        return name_.empty() ? mcp::target::numberName(id_) : name_;
    }

    std::string McpLink::numberName() const { return id_.empty() ? std::string() : mcp::target::numberName(id_); }

    bool McpLink::named() const
    {
        const std::lock_guard<std::mutex> lock(nameMutex_);
        return !name_.empty();
    }

    bool McpLink::setName(const std::string& name, std::string& why)
    {
        const std::string trimmed = juce::String::fromUTF8(name.c_str()).trim().toStdString();
        if (why = mcp::target::nameRefusal(trimmed, id_); !why.empty())
            return false;
        {
            const std::lock_guard<std::mutex> lock(nameMutex_);
            name_ = trimmed == numberName() ? std::string() : trimmed;
        }
        rescan();
        return true;
    }

    McpLink::View McpLink::view() const
    {
        const std::lock_guard<std::mutex> lock(viewMutex_);
        View v = view_;
        const auto now = std::chrono::steady_clock::now();
        v.running = !acting_.empty();
        v.tool = v.running ? acting_.back().first : lastTool_;
        v.ago = v.tool.empty() ? 0.0 : std::chrono::duration<double>(now - (v.running ? acting_.back().second : lastAt_)).count();
        return v;
    }

    void McpLink::choose(const std::string& id, bool on)
    {
        if (id_.empty() || dir_.empty())
            return;
        std::string token;
        {
            const std::lock_guard<std::mutex> lock(viewMutex_);
            if (id == id_)
                token = token_;
            for (const auto& [other, t] : tokens_)
                if (other == id)
                    token = t;
            if (token.empty())
                return;                                  // not open (any more)
            if (on)
                view_.chosen = id;
            else if (view_.chosen == id)
                view_.chosen.clear();
            ++view_.revision;
        }
        if (on)
            mcp::target::writeTarget(dir_, { id, token });
        else
            mcp::target::clearTarget(dir_, token);
        rescan();
    }

    void McpLink::rescan()
    {
        scanNow_.store(true);
        wake();
    }

    // This instance's card when it changed; the others', and the choice, read.
    void McpLink::scan()
    {
        mcp::target::Card mine{ token_, id_, name(), {}, {}, mcp::processId() };
        if (hooks_.place)
            std::tie(mine.track, mine.project) = hooks_.place();
        const std::string card = mine.token + '\n' + mine.name + '\n' + mine.track + '\n' + mine.project;
        // Written when it changed, or when it is gone (a server that found this number's socket refused, while it was
        // being taken again, removed the card of the one before).
        if ((card != written_ || !mcp::target::in(dir_, id_ + ".json").existsAsFile()) && mcp::target::writeCard(dir_, mine))
            written_ = card;
        std::vector<Peer> others;
        std::vector<std::pair<std::string, std::string>> tokens;
        for (const mcp::target::Card& c : mcp::target::cards(dir_))
            if (c.id != id_)
            {
                others.push_back({ c.id, c.name.empty() ? mcp::target::numberName(c.id) : c.name, c.track, c.project });
                tokens.push_back({ c.id, c.token });
            }
        const std::string chosen = mcp::target::chosen(dir_);
        const std::lock_guard<std::mutex> lock(viewMutex_);
        tokens_ = std::move(tokens);
        if (others != view_.others || chosen != view_.chosen)
        {
            view_.others = std::move(others);
            view_.chosen = chosen;
            ++view_.revision;
        }
    }

    void McpLink::began(const std::string& tool)
    {
        const std::lock_guard<std::mutex> lock(viewMutex_);
        acting_.push_back({ tool, std::chrono::steady_clock::now() });
    }

    void McpLink::ended(const std::string& tool)
    {
        const std::lock_guard<std::mutex> lock(viewMutex_);
        for (auto it = acting_.end(); it != acting_.begin();)
            if ((--it)->first == tool)
            {
                acting_.erase(it);
                break;
            }
        lastTool_ = tool;
        lastAt_ = std::chrono::steady_clock::now();
    }

    void McpLink::wake()
    {
        if (wake_[1] < 0)
            return;
        const char x = 1;
        [[maybe_unused]] const long long n = mcp::pairWrite(wake_[1], &x, 1);   // full: a poll thread already woken
    }

    void McpLink::run()
    {
        std::vector<std::shared_ptr<Client>> clients;
        while (!stop_.load())
        {
            std::vector<mcp::PollFd> fds;
            fds.push_back({ wake_[0], POLLIN, 0 });
            fds.push_back({ listen_, POLLIN, 0 });
            {
                const std::lock_guard<std::mutex> lock(outMutex_);
                for (const auto& c : clients)
                    fds.push_back({ c->reader.fd(), static_cast<short>(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0 });
            }
            const auto untilScan = std::chrono::duration_cast<std::chrono::milliseconds>(scanned_ + kScanEvery - std::chrono::steady_clock::now());
            const int r = mcp::pollFds(fds.data(), fds.size(),
                                       scanNow_.load() ? 0 : static_cast<int>(std::clamp<long long>(untilScan.count() + 1, 0, 1000)));
            if (r < 0)
            {
                if (mcp::interrupted())
                    continue;
                break;
            }
            if (scanNow_.exchange(false) || std::chrono::steady_clock::now() >= scanned_ + kScanEvery)
            {
                scan();
                scanned_ = std::chrono::steady_clock::now();
            }
            if (fds[0].revents != 0)
            {
                char drain[256];
                while (mcp::recvSome(wake_[0], drain, sizeof drain) > 0)
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
                if (const int fd = mcp::acceptOn(listen_); fd >= 0)
                {
                    mcp::noSigpipe(fd);
                    if (mcp::nonBlocking(fd))
                        clients.push_back(std::make_shared<Client>(fd));
                    else
                        mcp::closeFd(fd);
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
                i->setProperty("name", juce::String::fromUTF8(name().c_str()));
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
            {
                began(name);
                o.setProperty("result", resultOf(hooks_.call(name, arguments, 0)));
                ended(name);
            }
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
        began(name);
        l.thread = std::thread([this, client, id, name, arguments, call = l.call, done = l.done] {
            const ChatAgent::McpResult r = hooks_.call(name, arguments, call);
            ended(name);
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
            began(job.name);
            const ChatAgent::McpResult r = hooks_.call(job.name, job.arguments, job.call);
            ended(job.name);
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
