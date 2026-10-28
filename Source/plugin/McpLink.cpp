// Source/plugin/McpLink.cpp: see McpLink.h.
#include "plugin/McpLink.h"

#include "mcp/McpProtocol.h"

#include "Product.h"

#include <memory>
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
    }

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
        if (pipe(wake_) != 0)
        {
            close(listen_);
            listen_ = -1;
            unlink(path_.c_str());
            path_.clear();
            return;
        }
        thread_ = std::thread([this] { run(); });
    }

    McpLink::~McpLink()
    {
        stop_.store(true);
        if (wake_[1] >= 0)
        {
            const char x = 1;
            [[maybe_unused]] const ssize_t n = write(wake_[1], &x, 1);
        }
        if (thread_.joinable())
            thread_.join();
        if (listen_ >= 0)
        {
            close(listen_);
            unlink(path_.c_str());
        }
        for (const int fd : wake_)
            if (fd >= 0)
                close(fd);
    }

    void McpLink::run()
    {
        std::vector<std::unique_ptr<mcp::LineReader>> clients;
        std::vector<bool> spoke;                         // per client: it has sent a request (it counts as connected)
        while (!stop_.load())
        {
            std::vector<pollfd> fds;
            fds.push_back({ wake_[0], POLLIN, 0 });
            fds.push_back({ listen_, POLLIN, 0 });
            for (const auto& c : clients)
                fds.push_back({ c->fd(), POLLIN, 0 });
            const int r = poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
            if (r < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (fds[0].revents != 0 || stop_.load())
                break;
            const std::size_t polled = fds.size() - 2;   // the clients before this round's accept
            std::vector<bool> dead(clients.size(), false);
            for (std::size_t k = 0; k < polled; ++k)
            {
                if (fds[k + 2].revents == 0)
                    continue;
                mcp::LineReader& c = *clients[k];
                if (!c.readSome())
                {
                    dead[k] = true;
                    continue;
                }
                std::string line;
                while (!stop_.load() && c.take(line))
                {
                    if (line.empty())
                        continue;
                    if (!spoke[k])
                    {
                        spoke[k] = true;
                        ++talking_;
                    }
                    if (!mcp::writeLine(c.fd(), answer(line)))
                    {
                        dead[k] = true;
                        break;
                    }
                }
            }
            for (std::size_t k = dead.size(); k-- > 0;)
                if (dead[k])
                {
                    close(clients[k]->fd());
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(k));
                    if (spoke[k])
                        --talking_;
                    spoke.erase(spoke.begin() + static_cast<std::ptrdiff_t>(k));
                }
            if ((fds[1].revents & POLLIN) != 0)
                if (const int fd = accept(listen_, nullptr, nullptr); fd >= 0)
                {
                    mcp::noSigpipe(fd);
                    clients.push_back(std::make_unique<mcp::LineReader>(fd));
                    spoke.push_back(false);
                }
        }
        for (const auto& c : clients)
            close(c->fd());
    }

    std::string McpLink::answer(const std::string& line)
    {
        const juce::var request = juce::JSON::parse(juce::String::fromUTF8(line.c_str()));
        auto* reply = new juce::DynamicObject();
        const juce::var out(reply);
        reply->setProperty("id", request.getProperty("id", juce::var()));
        const juce::String method = request["method"].toString();
        if (method == "info")
        {
            juce::var info = hooks_.info ? hooks_.info() : juce::var(new juce::DynamicObject());
            if (juce::DynamicObject* o = info.getDynamicObject())
            {
                o->setProperty("id", juce::String(id_));
                o->setProperty("name", juce::String(name_));
            }
            reply->setProperty("result", info);
        }
        else if (method == "tools")
            reply->setProperty("result", ChatAgent::mcpTools());
        else if (method == "guide")
            reply->setProperty("result", juce::String::fromUTF8(ChatAgent::mcpGuide().c_str()));
        else if (method == "call" && hooks_.call)
        {
            const juce::var params = request["params"];
            const ChatAgent::McpResult r = hooks_.call(params["name"].toString().toStdString(), params["arguments"]);
            auto* res = new juce::DynamicObject();
            res->setProperty("content", r.content);
            res->setProperty("isError", r.isError);
            reply->setProperty("result", juce::var(res));
        }
        else
            reply->setProperty("error", "there is no method \"" + method + "\"");
        return juce::JSON::toString(out, true).toStdString();
    }
}
