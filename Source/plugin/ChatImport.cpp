// Source/plugin/ChatImport.cpp: write_file (ChatAgent.h), and the program's files in and out through it. A file written
// whole (name, content); several written as one change, all or none (files); a file or a folder on this Mac that the
// user named brought into the program's files (from: the program files there, text, kMaxImportBytes each, names the
// editor takes, all or none, without the folder sync's read-only copies); and the editor's files written to a folder of
// their own on this Mac (to: in exportsFolder(), the user's Music folder's <Company>/<Name>/Programs, a probe's sandbox
// under CTest), which only adds files: a name taken gets a number after it, and nothing there is replaced or deleted.
// Every write but the export goes into the editor as one change (one undo step there); an import reads, it never
// writes or moves the user's files.
#include "plugin/ChatAgent.h"

#include "fminus/CirDoc.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

namespace playground
{
    namespace
    {
        constexpr std::size_t kMaxImportBytes = 256 * 1024;   // a file brought in at most
        constexpr std::size_t kMaxFolderName = 64;            // an export's folder name at most

        int lineCount(const std::string& t)
        {
            if (t.empty())
                return 0;
            return static_cast<int>(std::count(t.begin(), t.end(), '\n')) + (t.back() == '\n' ? 0 : 1);
        }

        int layoutLines(const std::string& text)
        {
            int n = 0;
            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t end = std::min(text.find('\n', start), text.size());
                n += cir::isLayoutLine(std::string_view(text).substr(start, end - start)) ? 1 : 0;
                start = end + 1;
            }
            return n;
        }

        // A netlist written without its drawing keeps the old one (its *@ lines, put back before .end): the places the
        // user gave its parts stay; a line naming a part that is gone goes at the next drawing. `kept`: how many lines.
        std::string withOldDrawing(const std::string& name, const std::string& old, const std::string& content, int& kept)
        {
            kept = 0;
            if (!isCircuitFile(name) || layoutLines(content) != 0 || (kept = layoutLines(old)) == 0)
                return content;
            std::string block;
            std::size_t at = 0;
            while (at <= old.size())
            {
                const std::size_t end = std::min(old.find('\n', at), old.size());
                std::string_view line = std::string_view(old).substr(at, end - at);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                if (cir::isLayoutLine(line))
                    block += std::string(line) + "\n";
                at = end + 1;
            }
            return cir::withBlock(content, block);
        }

        std::string lowered(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            return s;
        }

        // A program file's name: one of the editor's kinds (editor/Items.h: .fm, .cir, .gui, .sc), as FolderSync and the
        // headless renderer take them.
        bool programExtension(const std::string& name)
        {
            const std::size_t dot = name.rfind('.');
            const std::string ext = dot == std::string::npos ? std::string() : lowered(name.substr(dot));
            return ext == ".fm" || ext == ".cir" || ext == ".gui" || ext == ".sc";
        }

        std::string nameRule()
        {
            return "letters, digits, _, - and ., not starting with '.', at most " + std::to_string(kMaxNameLength) + " characters";
        }

        // Text: no NUL byte, and UTF-8 throughout (a binary file is neither).
        bool isText(std::string_view t)
        {
            for (std::size_t i = 0; i < t.size();)
            {
                const auto c = static_cast<unsigned char>(t[i]);
                if (c == 0)
                    return false;
                const int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
                if (more < 0 || (more == 1 && c < 0xC2) || i + static_cast<std::size_t>(more) >= t.size() + (more == 0 ? 1 : 0))
                    return false;
                for (int k = 1; k <= more; ++k)
                    if ((static_cast<unsigned char>(t[i + static_cast<std::size_t>(k)]) & 0xC0) != 0x80)
                        return false;
                i += static_cast<std::size_t>(more) + 1;
            }
            return true;
        }

        // The characters the editor does not hold (TextEditor's rule: printable ASCII, tabs and line ends): what its
        // sanitise makes '?'.
        int outsideAscii(std::string_view t)
        {
            int n = 0;
            for (const char ch : t)
            {
                const auto c = static_cast<unsigned char>(ch);
                if ((c < 32 && c != '\t' && c != '\n' && c != '\r') || c == 127 || (c >= 0xC0))
                    ++n;
            }
            return n;
        }

        // The folder an export goes into (the user's Music folder's <Company>/<Name>/Programs, beside trace's Renders);
        // a probe's sandbox's programs/ under CTest, never the user's folder.
        juce::File exportsFolder()
        {
            const std::string sandbox = std::string(product::kEnvPrefix) + "PREFS_DIR";
            if (const char* dir = std::getenv(sandbox.c_str()); dir != nullptr && *dir != '\0')
                return juce::File(juce::String::fromUTF8(dir)).getChildFile("programs");
            return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                .getChildFile(product::kCompany)
                .getChildFile(product::kName)
                .getChildFile("Programs");
        }

        // The read-only copies a folder sync wrote (its manifest's copy lines; FolderSync.h): not the program's own.
        std::set<std::string> syncCopies(const juce::File& dir)
        {
            std::set<std::string> out;
            const juce::File manifest = dir.getChildFile(".fmsync");
            if (!manifest.existsAsFile() || manifest.getSize() > (1 << 20))
                return out;
            juce::StringArray lines;
            manifest.readLines(lines);
            if (lines.isEmpty() || !lines[0].startsWith("fmsync 1"))
                return out;
            for (const juce::String& l : lines)
            {
                const std::string line = l.toStdString();
                if (line.size() > 22 && line.rfind("copy ", 0) == 0 && line[21] == ' ')
                    out.insert(line.substr(22));
            }
            return out;
        }

        struct Incoming
        {
            std::string name, text;
            int replaced = 0;                          // characters outside ASCII the editor holds as '?'
            bool lineEnds = false;                     // CR LF (or CR) line ends, read as LF
        };

        // The program files of `given` (a file, or a folder's), checked; "" or why none is taken. `left`: what a folder
        // holds that is no program file of its own.
        std::string readFrom(const std::string& given, std::vector<Incoming>& out, std::vector<std::string>& left, std::string& where)
        {
            std::string path = juce::String(given).trim().toStdString();
            if (path.rfind("~/", 0) == 0 || path == "~")
                path = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName().toStdString() + path.substr(1);
            if (path.empty() || !juce::File::isAbsolutePath(juce::String::fromUTF8(path.c_str())))
                return "from names a file or a folder on this Mac, whole (/Users/... or ~/...): '" + given + "' is not one";
            const juce::File named(juce::String::fromUTF8(path.c_str()));
            where = named.getFullPathName().toStdString();
            std::vector<juce::File> files;
            std::set<std::string> copies;
            if (named.existsAsFile())
            {
                const std::string n = named.getFileName().toStdString();
                if (!programExtension(n))
                    return n + " is not a program file (.fm, .cir, .gui or .sc)";
                files.push_back(named);
            }
            else if (named.isDirectory())
            {
                copies = syncCopies(named);
                for (const juce::DirectoryEntry& e : juce::RangedDirectoryIterator(named, false, "*", juce::File::findFiles))
                {
                    const std::string n = e.getFile().getFileName().toStdString();
                    if (n.empty() || n[0] == '.')
                        continue;                      // hidden: the folder sync's own, an editor's
                    if (programExtension(n))
                        files.push_back(e.getFile());
                    else
                        left.push_back(n);
                }
                std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
                    const bool pa = a.getFileName() == kProgramName, pb = b.getFileName() == kProgramName;
                    return pa != pb ? pa : a.getFileName() < b.getFileName();
                });
                std::sort(left.begin(), left.end());
                if (files.empty())
                    return "the folder " + where + " holds no program file (.fm, .cir, .gui or .sc)";
            }
            else
                return "there is no file or folder " + where;

            std::string problems;
            for (const juce::File& f : files)
            {
                const std::string n = f.getFileName().toStdString();
                if (!validFileName(n))
                {
                    problems += "\n- '" + n + "' cannot name a file in the editor (" + nameRule() + ")";
                    continue;
                }
                const juce::int64 bytes = f.getSize();
                if (bytes < 0 || static_cast<std::size_t>(bytes) > kMaxImportBytes)
                {
                    problems += "\n- " + n + " is " + std::to_string(bytes / 1024) + " KB, past the " + std::to_string(kMaxImportBytes / 1024)
                              + " KB a file brought in may be";
                    continue;
                }
                juce::MemoryBlock mb;
                if (!f.loadFileAsData(mb))
                {
                    problems += "\n- " + n + " could not be read";
                    continue;
                }
                Incoming in{ n, std::string(static_cast<const char*>(mb.getData()), mb.getSize()) };
                if (!isText(in.text))
                {
                    problems += "\n- " + n + " is not text (it holds a NUL byte, or bytes that are not UTF-8)";
                    continue;
                }
                // The plugin's own text, as this build or a folder sync wrote it: not the program's.
                const LibraryFile* lib = findLibraryFile(n);
                const bool copy = copies.count(n) != 0;
                if ((n == "host.fm" && in.text == Patch::hostSource()) || (copy && lib != nullptr && in.text == lib->source))
                {
                    left.push_back(n + " (the plugin's own)");
                    continue;
                }
                if (copy)
                {
                    left.push_back(n + " (the folder sync's read-only copy)");
                    continue;
                }
                in.replaced = outsideAscii(in.text);
                in.lineEnds = in.text.find('\r') != std::string::npos;
                if (in.replaced > 0 || in.lineEnds)
                    in.text = funkgui::TextEditor::sanitise(in.text);
                out.push_back(std::move(in));
            }
            if (!problems.empty())
            {
                out.clear();
                return "nothing was brought in:" + problems;
            }
            if (out.empty())
                return "nothing in " + where + " is the program's own: " + [&] {
                    std::string s;
                    for (const std::string& l : left)
                        s += (s.empty() ? "" : ", ") + l;
                    return s;
                }();
            return {};
        }
    }

    std::string ChatAgent::writeFileText(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        const auto note = [this](std::string t) {
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, std::move(t));
        };
        const auto has = [&](const char* k) { return !input.getProperty(k, juce::var()).isVoid(); };
        const int forms = (has("files") ? 1 : 0) + (has("from") ? 1 : 0) + (has("to") ? 1 : 0);
        if (forms > 1 || (forms == 1 && (has("name") || has("content"))))
            return fail("write_file takes one of: name and content, files, from or to");
        const auto findIn = [](const Draft& d, const std::string& n) {
            for (std::size_t i = 0; i < d.files.size(); ++i)
                if (d.files[i].name == n)
                    return static_cast<int>(i);
            return -1;
        };

        // ---- to: the editor's files, out to a folder of their own ------------------------------------------------------
        if (has("to"))
        {
            const juce::var toVar = input.getProperty("to", juce::var());
            const std::string to = juce::String(toVar.toString()).trim().toStdString();
            bool good = toVar.isString() && !to.empty() && to.size() <= kMaxFolderName && to[0] != '.';
            for (const char c : to)
                good = good && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_'
                                || c == '-' || c == '.');
            if (!good)
                return fail("to is the export's folder name: letters, digits, spaces, _, - and ., not starting with '.', at most "
                            + std::to_string(kMaxFolderName) + " characters (a name, not a path: it goes in the plugin's "
                              "Programs folder on this Mac)");
            const Draft d = host_.chatDraft();
            if (d.files.empty())
                return fail("The editor has no file to export.");
            // Two names that differ only in case are one file on this Mac's disk (APFS ignores case by default): the
            // second would replace the first.
            {
                std::map<std::string, std::string> seen;
                for (const SourceFile& f : d.files)
                    if (const auto [it, added] = seen.emplace(lowered(f.name), f.name); !added)
                        return fail(it->second + " and " + f.name + " differ only in case, which this Mac's disk takes as one "
                                    "name: rename one (rename_file) and export again. Nothing was exported.");
            }
            const juce::File base = exportsFolder();
            if (!base.isDirectory() && !base.createDirectory().wasOk())
                return fail("Could not make the folder " + base.getFullPathName().toStdString() + ".");
            std::string stem = to;
            for (int k = 2; base.getChildFile(juce::String::fromUTF8(stem.c_str())).exists(); ++k)
                stem = to + " " + std::to_string(k);
            const juce::File dir = base.getChildFile(juce::String::fromUTF8(stem.c_str()));
            if (!dir.createDirectory().wasOk())
                return fail("Could not make the folder " + dir.getFullPathName().toStdString() + ".");
            std::string listed;
            for (const SourceFile& f : d.files)
            {
                const juce::File target = dir.getChildFile(juce::String::fromUTF8(f.name.c_str()));
                juce::TemporaryFile temp(target);
                const bool there = target.exists();    // another of its name, as this disk compares names: never replaced
                const bool wrote = !there && temp.getFile().replaceWithData(f.text.data(), f.text.size())
                                && temp.overwriteTargetFileWithTemporary();
                if (!wrote)
                {
                    dir.deleteRecursively();           // the folder this export made, and nothing else
                    return fail("Writing " + f.name + " into " + dir.getFullPathName().toStdString() + " failed ("
                                + (there ? "this disk holds a file of that name there already" : "is the disk full?")
                                + "): nothing was exported.");
                }
                const int lines = lineCount(f.text);
                listed += "\n- " + f.name + " (" + std::to_string(lines) + (lines == 1 ? " line)" : " lines)");
            }
            note("exported the files to " + stem);
            return "Exported the editor's " + std::to_string(d.files.size()) + (d.files.size() == 1 ? " file" : " files")
                 + " as they are now, on this Mac (the user can open them), to " + dir.getFullPathName().toStdString()
                 + (stem != to ? " (\"" + to + "\" was taken, and nothing is replaced)" : std::string()) + ":" + listed
                 + "\nIt changed nothing in the plugin.";
        }

        // ---- files and from: into the editor, as one change --------------------------------------------------------------
        if (has("files") || has("from"))
        {
            std::vector<Incoming> incoming;
            std::vector<std::string> left;
            std::string where;
            const bool fromDisk = has("from");
            if (fromDisk)
            {
                const juce::var f = input.getProperty("from", juce::var());
                if (!f.isString())
                    return fail("from is a path: a file or a folder on this Mac that the user named");
                if (const std::string why = readFrom(f.toString().toStdString(), incoming, left, where); !why.empty())
                    return fail(where.empty() ? "Nothing was brought in: " + why : "From " + where + ": " + why);
            }
            else
            {
                const juce::Array<juce::var>* list = input.getProperty("files", juce::var()).getArray();
                if (list == nullptr || list->isEmpty() || list->size() > static_cast<int>(kMaxFiles))
                    return fail("files is a list of 1 to " + std::to_string(kMaxFiles) + " files, [{name, content}]");
                for (int k = 0; k < list->size(); ++k)
                {
                    const juce::var& e = list->getReference(k);
                    const juce::var n = e.getProperty("name", juce::var()), c = e.getProperty("content", juce::var());
                    if (!n.isString() || !c.isString())
                        return fail("files[" + std::to_string(k) + "] needs a name and the content");
                    incoming.push_back({ n.toString().toStdString(), c.toString().toStdString(), 0, false });
                    if (!validFileName(incoming.back().name))
                        return fail("files[" + std::to_string(k) + "]: '" + incoming.back().name + "' cannot name a file: use "
                                    + nameRule() + ". Nothing was written");
                }
            }
            std::set<std::string> names;
            for (const Incoming& in : incoming)
                if (!names.insert(in.name).second)
                    return fail(in.name + " comes twice. Nothing was written");
            Draft d = host_.chatDraft();
            std::size_t added = 0;
            for (const Incoming& in : incoming)
                added += findIn(d, in.name) < 0 ? 1 : 0;
            if (d.files.size() + added > kMaxFiles)
                return fail("The editor would hold " + std::to_string(d.files.size() + added) + " files, past its "
                            + std::to_string(kMaxFiles) + ": delete some first. Nothing was written");
            std::string listed;
            int shown = -1;
            std::vector<std::pair<std::string, std::string>> written;   // name, text
            std::set<std::string> before;
            for (const SourceFile& f : d.files)
                before.insert(f.name);
            for (const Incoming& in : incoming)
            {
                int i = findIn(d, in.name);
                int kept = 0;
                std::string text = in.text;
                if (i < 0)
                {
                    d.files.push_back({ in.name, text });
                    i = static_cast<int>(d.files.size()) - 1;
                }
                else
                {
                    if (!fromDisk)
                        text = withOldDrawing(in.name, d.files[static_cast<std::size_t>(i)].text, in.text, kept);
                    d.files[static_cast<std::size_t>(i)].text = text;
                }
                if (shown < 0)
                    shown = i;
                written.emplace_back(in.name, text);
                const int lines = lineCount(text);
                listed += "\n- " + in.name + " (" + std::to_string(lines) + (lines == 1 ? " line" : " lines") + "; "
                        + (before.count(in.name) == 0 ? "new" : "replaced");
                if (kept > 0)
                    listed += "; kept the drawing's " + std::to_string(kept) + " *@ lines";
                if (in.lineEnds)
                    listed += "; CR LF line ends read as LF";
                if (in.replaced > 0)
                    listed += "; " + std::to_string(in.replaced) + (in.replaced == 1 ? " character" : " characters")
                            + " outside ASCII became '?', as the editor holds ASCII";
                listed += ")";
            }
            d.active = shown;
            setActivity(fromDisk ? "Bringing files in" : "Writing files");
            host_.chatSetDraft(d);
            std::string versions;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [n, t] : written)
                {
                    if (!fromDisk)
                        markSeen(n, t);                // Claude wrote it; a file from disk goes whole with the next state
                    versions += (versions.empty() ? "" : ", ") + n + " " + keepVersion(n, t);
                }
            }
            std::string out;
            if (fromDisk)
            {
                note("brought in " + std::to_string(written.size()) + (written.size() == 1 ? " file" : " files") + " from "
                     + juce::File(juce::String::fromUTF8(where.c_str())).getFileName().toStdString());
                out = "Brought " + std::to_string(written.size()) + (written.size() == 1 ? " file" : " files") + " into the editor from "
                    + where + ", as one change the user can undo:" + listed;
                if (!left.empty())
                {
                    out += "\nLeft out:";
                    for (const std::string& l : left)
                        out += " " + l + ",";
                    out.back() = '.';
                    out += " (Only program files come in: .fm, .cir, .gui, .sc.)";
                }
                out += "\nRead them before you edit them (read_file); call run_program to hear them.";
            }
            else
            {
                note("wrote " + std::to_string(written.size()) + " files");
                out = "Wrote " + std::to_string(written.size()) + " files as one change the user can undo:" + listed
                    + "\nA write does not play anything: call run_program after the files are ready.";
            }
            return out + "\nVersions: " + versions + ".";
        }

        // ---- name and content: one file written whole ------------------------------------------------------------------
        std::string file, content;
        {
            const juce::var n = input.getProperty("name", juce::var()), c = input.getProperty("content", juce::var());
            if (!n.isString() || !c.isString())
                return fail("write_file needs a name and the content");
            file = n.toString().toStdString();
            content = c.toString().toStdString();
        }
        if (!validFileName(file))
            return fail("'" + file + "' cannot name a file: use " + nameRule());
        Draft d = host_.chatDraft();
        int i = findIn(d, file);
        int keptLines = 0;
        if (i < 0)
        {
            if (d.files.size() >= kMaxFiles)
                return fail("The editor holds at most " + std::to_string(kMaxFiles) + " files; delete one first");
            d.files.push_back({ file, content });
            i = static_cast<int>(d.files.size()) - 1;
        }
        else
        {
            content = withOldDrawing(file, d.files[static_cast<std::size_t>(i)].text, content, keptLines);
            d.files[static_cast<std::size_t>(i)].text = content;
        }
        d.active = i;
        setActivity("Writing " + file);
        host_.chatSetDraft(d);
        std::string version;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            markSeen(file, content);
            version = keepVersion(file, content);
        }
        const int lines = lineCount(content);
        const std::string count = std::to_string(lines) + (lines == 1 ? " line" : " lines");
        note("wrote " + file + " (" + count + ")");
        std::string out = "Wrote " + file + " (" + count + "; version " + version + ").";
        if (keptLines > 0)
            out += " Kept the drawing's " + std::to_string(keptLines) + " *@ lines; send edit_circuit 'layout' to redraw it.";
        bool included = i == 0;
        for (const SourceFile& f : d.files)
            if (f.text.find("#include \"" + file + "\"") != std::string::npos)
                included = true;
        if (!included)
            out += " No file #includes it yet.";
        return out;
    }
}
