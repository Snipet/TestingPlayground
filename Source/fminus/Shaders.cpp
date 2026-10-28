// Source/fminus/Shaders.cpp: see Shaders.h.
#include "fminus/Shaders.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace playground
{
    namespace
    {
        constexpr int kTimeoutMs = 10000;            // shaderc on a shader the plugin's size takes about 60 ms

        bool identifier(const std::string& s)
        {
            if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
                return false;
            return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        }

        // An input's name as a shader name: letters, digits and _ ("in_cutoff").
        std::string inputName(const std::string& name)
        {
            std::string out;
            for (const char c : name)
                out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            return out;
        }

        bool writeFile(const std::filesystem::path& p, const std::string& text)
        {
            std::ofstream f(p, std::ios::binary);
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            return static_cast<bool>(f);
        }

        std::string readFile(const std::filesystem::path& p)
        {
            std::ifstream f(p, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }

        // Runs argv[0] with its output (both streams) into `log`; its exit status, or -1 when it could not run or took
        // longer than kTimeoutMs (it is killed).
        int run(const std::vector<std::string>& args, const std::filesystem::path& log)
        {
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            posix_spawn_file_actions_adddup2(&actions, 1, 2);
            posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
            std::vector<char*> argv;
            for (const std::string& a : args)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            pid_t pid = 0;
            const int spawned = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            if (spawned != 0)
                return -1;
            const auto start = std::chrono::steady_clock::now();
            for (;;)
            {
                int status = 0;
                const pid_t done = waitpid(pid, &status, WNOHANG);
                if (done == pid)
                    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                if (done < 0 && errno != EINTR)
                    return -1;
                if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(kTimeoutMs))
                {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    return -1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        // shaderc's log as a note: its first ERROR's message, on the line of the file it is about. shaderc reports
        // lines of its own rewritten listing (bgfx's HLSL), so the line is found in the file instead: the first one
        // with the name the error names, else the one whose text the listing's marked line has.
        std::string noteOf(const std::string& file, const std::string& body, int bodyLine, const std::string& log)
        {
            std::istringstream in(log);
            std::string line, message, marked;
            int at = 0;
            while (std::getline(in, line))
            {
                if (marked.empty() && line.rfind(">>> ", 0) == 0)
                {
                    const std::size_t colon = line.find(": ");
                    if (colon != std::string::npos && std::atoi(line.c_str() + 4) > 0)
                        marked = line.substr(colon + 2);
                }
                if (message.empty() && line.rfind("ERROR: ", 0) == 0)
                {
                    // ERROR: 0:422: 'undefinedThing' : unknown variable
                    std::size_t p = line.find(':', 7);
                    p = p == std::string::npos ? std::string::npos : line.find(':', p + 1);
                    message = p == std::string::npos ? line.substr(7) : line.substr(p + 1);
                    while (!message.empty() && message.front() == ' ')
                        message.erase(message.begin());
                    while (!message.empty() && (message.back() == ' ' || message.back() == '\r'))
                        message.pop_back();
                }
            }
            if (message.empty())
            {
                // Not glslang's: shaderc's own line (a missing $input, a file it cannot read), or nothing.
                std::istringstream again(log);
                while (std::getline(again, line))
                    if (!line.empty() && line.find("Failed to build shader") == std::string::npos && line != "Code:"
                        && line != "---")
                        message = line;
                if (message.empty())
                    message = "shaderc could not run";
            }
            // The name in quotes, when the error names one.
            std::string name;
            if (const std::size_t q = message.find('\''); q != std::string::npos)
                if (const std::size_t e = message.find('\'', q + 1); e != std::string::npos)
                    name = message.substr(q + 1, e - q - 1);
            std::vector<std::string> lines;
            {
                std::istringstream b(body);
                while (std::getline(b, line))
                    lines.push_back(line);
            }
            const auto word = [](const std::string& s, const std::string& w) {
                for (std::size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1))
                {
                    const bool before = p == 0 || !(std::isalnum(static_cast<unsigned char>(s[p - 1])) || s[p - 1] == '_');
                    const std::size_t end = p + w.size();
                    const bool after = end >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_');
                    if (before && after)
                        return true;
                }
                return false;
            };
            if (identifier(name))
                for (std::size_t k = 0; k < lines.size() && at == 0; ++k)
                    if (word(lines[k], name))
                        at = static_cast<int>(k) + 1;
            if (at == 0 && !marked.empty())
            {
                // The listing's line in bgfx's words, back in the shader's: the longest of the file's lines it holds.
                std::string m = marked;
                const auto swap = [&m](const std::string& from, const std::string& to) {
                    for (std::size_t p = m.find(from); p != std::string::npos; p = m.find(from, p + to.size()))
                        m.replace(p, from.size(), to);
                };
                swap("bgfx_FragData0", "gl_FragColor");
                swap("float4", "vec4");
                swap("float3", "vec3");
                swap("float2", "vec2");
                std::size_t best = 0;
                for (std::size_t k = 0; k < lines.size(); ++k)
                {
                    std::string t = lines[k];
                    t.erase(0, t.find_first_not_of(" \t"));
                    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r'))
                        t.pop_back();
                    if (t.size() > 3 && t.size() > best && m.find(t) != std::string::npos)
                    {
                        best = t.size();
                        at = static_cast<int>(k) + 1;
                    }
                }
            }
            (void)bodyLine;
            return file + ":" + std::to_string(at > 0 ? at : 1) + ":1: note: the shader does not compile: " + message
                 + " (it draws nothing until it does)\n";
        }
    }

    std::string shaderSource(const std::string& body, const std::vector<std::string>& inputs, const ShaderTools& tools,
                             int* bodyLine)
    {
        std::string s = tools.prelude;
        if (!s.empty() && s.back() != '\n')
            s += '\n';
        s += "// The plugin's names (Source/fminus/Shaders.h).\n"
             "#define u_time (u_viewSize.w)\n"
             "#define u_size (u_rect.zw)\n";
        static constexpr const char* kSwizzle[4] = { "x", "y", "z", "w" };
        for (int k = 0; k < 8; ++k)
            s += "#define in" + std::to_string(k) + " (u_params[" + std::to_string(k / 4) + "]." + kSwizzle[k % 4] + ")\n";
        for (std::size_t k = 0; k < inputs.size() && k < 8; ++k)
            if (!inputs[k].empty())
                s += "#define in_" + inputName(inputs[k]) + " in" + std::to_string(k) + "\n";
        if (bodyLine != nullptr)
            *bodyLine = 1 + static_cast<int>(std::count(s.begin(), s.end(), '\n'));
        s += body;
        if (s.empty() || s.back() != '\n')
            s += '\n';
        return s;
    }

    ShaderBuild buildShader(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                            const ShaderTools& tools)
    {
        ShaderBuild out;
        if (!tools.usable())
        {
            out.error = file + ":1:1: note: this plugin cannot build shaders (no shaderc beside it): the shader draws "
                               "nothing\n";
            return out;
        }
        std::error_code ec;
        const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
        std::string pattern = (tmp / "tp-shader-XXXXXX").string();
        if (ec || mkdtemp(pattern.data()) == nullptr)
        {
            out.error = file + ":1:1: note: the shader cannot be built: no temporary folder\n";
            return out;
        }
        const std::filesystem::path dir = pattern;
        int bodyLine = 1;
        const std::string source = shaderSource(body, inputs, tools, &bodyLine);
        const bool written = writeFile(dir / "fs.sc", source) && writeFile(dir / "varying.def.sc", tools.varyingDef)
                          && (tools.bgfxShaderSh.empty() || writeFile(dir / "bgfx_shader.sh", tools.bgfxShaderSh));
        if (!written)
            out.error = file + ":1:1: note: the shader cannot be built: its files cannot be written\n";
        else
        {
            const int status = run({ tools.shaderc, "-f", (dir / "fs.sc").string(), "-o", (dir / "fs.bin").string(),
                                     "--type", "fragment", "--platform", tools.platform, "-p", tools.profile,
                                     "--varyingdef", (dir / "varying.def.sc").string(), "-i", dir.string() },
                                   dir / "log.txt");
            const std::string bin = status == 0 ? readFile(dir / "fs.bin") : std::string();
            if (!bin.empty())
                out.binary.assign(bin.begin(), bin.end());
            else
                out.error = status < 0 && readFile(dir / "log.txt").empty()
                              ? file + ":1:1: note: the shader cannot be built: shaderc did not run\n"
                              : noteOf(file, body, bodyLine, readFile(dir / "log.txt"));
        }
        std::filesystem::remove_all(dir, ec);
        return out;
    }

    ShaderBuild ShaderCache::build(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                                   const ShaderTools& tools)
    {
        const std::string key = file + '\n' + shaderSource(body, inputs, tools) + tools.platform + tools.profile + tools.shaderc;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (const auto it = builds_.find(key); it != builds_.end())
            {
                it->second.first = ++clock_;
                return it->second.second;
            }
        }
        ShaderBuild b = buildShader(file, body, inputs, tools);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (builds_.size() >= 32)
        {
            auto oldest = builds_.begin();
            for (auto it = builds_.begin(); it != builds_.end(); ++it)
                if (it->second.first < oldest->second.first)
                    oldest = it;
            builds_.erase(oldest);
        }
        builds_[key] = { ++clock_, b };
        return b;
    }

    uint32_t shaderId(const std::vector<uint8_t>& binary)
    {
        uint32_t h = 2166136261u;                    // FNV-1a
        for (const uint8_t b : binary)
            h = (h ^ b) * 16777619u;
        return h == 0 ? 1u : h;
    }
}
