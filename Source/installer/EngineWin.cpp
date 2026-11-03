// Source/installer/EngineWin.cpp: the engine's Windows half (Engine.h says what differs there; EnginePlatform.h): what
// is at a path (a symbolic link or a junction is a link, never followed), who owns it and who may move it (the file's
// owner and the system's own access check), renames (MoveFileEx, never over anything), versions (a VST3's
// moduleinfo.json, an app's version resource), this computer's places and folders, and "All users": one PowerShell
// script run through UAC, whose exit code is its report, and what it leaves in its staging folders moved to the
// Recycle Bin as this user afterwards.
#include "installer/Engine.h"
#include "installer/EnginePlatform.h"

#include <juce_core/juce_core.h>

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <winver.h>
#undef near                                          // windows.h's 16-bit leftovers
#undef far

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace playground::installer
{
    namespace pf = platform;

    namespace
    {
        std::string utf8(const std::wstring& w)
        {
            if (w.empty())
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<std::size_t>(std::max(n, 0)), '\0');
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        // A path as UTF-8, with Windows' separators.
        std::string pathText(fs::path p)
        {
            return utf8(p.make_preferred().wstring());
        }

        // The user this process runs as: its SID, and as text ("S-1-5-21-...").
        struct UserSid
        {
            std::vector<BYTE> sid;
            std::string text;
        };

        const UserSid& thisUser()
        {
            static const UserSid user = [] {
                UserSid u;
                HANDLE token = nullptr;
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                    return u;
                DWORD n = 0;
                GetTokenInformation(token, TokenUser, nullptr, 0, &n);
                std::vector<BYTE> buf(n);
                if (n > 0 && GetTokenInformation(token, TokenUser, buf.data(), n, &n))
                {
                    const auto* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
                    const auto* sid = static_cast<const BYTE*>(tu->User.Sid);
                    u.sid.assign(sid, sid + GetLengthSid(tu->User.Sid));
                    LPWSTR text = nullptr;
                    if (ConvertSidToStringSidW(tu->User.Sid, &text))
                    {
                        u.text = utf8(text);
                        LocalFree(text);
                    }
                }
                CloseHandle(token);
                return u;
            }();
            return user;
        }

        // An account's name ("Administrators", "SYSTEM", a user's), else its SID as text.
        std::string accountName(PSID sid)
        {
            wchar_t name[256], domain[256];
            DWORD nn = 256, dn = 256;
            SID_NAME_USE use{};
            if (LookupAccountSidW(nullptr, sid, name, &nn, domain, &dn, &use))
                return utf8(name);
            LPWSTR text = nullptr;
            std::string out = "another account";
            if (ConvertSidToStringSidW(sid, &text))
            {
                out = utf8(text);
                LocalFree(text);
            }
            return out;
        }

        fs::path knownFolder(REFKNOWNFOLDERID id)
        {
            PWSTR p = nullptr;
            fs::path out;
            if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p != nullptr)
                out = fs::path(p);
            CoTaskMemFree(p);
            return out;
        }

        std::string randomText(std::size_t n)
        {
            static const char kAlnum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            std::vector<unsigned char> r(n);
            BCryptGenRandom(nullptr, r.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            std::string s;
            for (const unsigned char b : r)
                s += kAlnum[b % 62];
            return s;
        }

        // A staging folder's name for step `k` of the run named `run`: .installing-XXXXXX (as every staging folder is,
        // so that sweep() knows one an interrupted run left), the same in the script and when the engine empties it.
        std::string stageName(const std::string& run, int k)
        {
            static const char kAlnum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            uint64_t h = 1469598103934665603ull;         // FNV-1a
            for (const char c : run + "/" + std::to_string(k))
            {
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ull;
            }
            std::string s = ".installing-";
            for (int i = 0; i < 6; ++i)
            {
                s += kAlnum[h % 62];
                h /= 62;
            }
            return s;
        }

        std::string lowerAscii(std::string s)
        {
            for (char& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        }

        // The known hosts, by their executable's name (its start, any case, then nothing, a space, a dot, a dash or a
        // digit): a running one is told to restart.
        struct Host
        {
            const char* exe;
            const char* name;
        };
        constexpr Host kHosts[] = {
            { "reaper", "REAPER" }, { "Ableton Live", "Ableton Live" }, { "Live", "Ableton Live" },
            { "Bitwig Studio", "Bitwig Studio" }, { "Cubase", "Cubase" }, { "Nuendo", "Nuendo" },
            { "Studio One", "Studio One" }, { "FL64", "FL Studio" }, { "FL", "FL Studio" }, { "Reason", "Reason" },
            { "Waveform", "Waveform" }, { "Tracktion", "Tracktion" }, { "Renoise", "Renoise" }, { "Audacity", "Audacity" },
            { "AudioPluginHost", "AudioPluginHost" }, { "Ardour", "Ardour" }, { "Mixbus", "Mixbus" },
            { "Cakewalk", "Cakewalk" }, { "Digital Performer", "Digital Performer" }, { "LUNA", "Luna" },
        };

        // A PE's version: its version resource's ProductVersion (the translations it lists, then the usual tables),
        // else its fixed version numbers; "" when it has none.
        std::string peVersion(const fs::path& pe)
        {
            DWORD ignored = 0;
            const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, pe.c_str(), &ignored);
            if (size == 0)
                return {};
            std::vector<unsigned char> data(size);
            if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, pe.c_str(), 0, size, data.data()))
                return {};
            struct Translation
            {
                WORD language, codePage;
            };
            std::vector<std::wstring> tables;
            Translation* tr = nullptr;
            UINT trBytes = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &trBytes)
                && tr != nullptr)
                for (UINT k = 0; k < trBytes / sizeof(Translation); ++k)
                {
                    wchar_t b[16];
                    std::swprintf(b, 16, L"%04x%04x", static_cast<unsigned>(tr[k].language), static_cast<unsigned>(tr[k].codePage));
                    tables.emplace_back(b);
                }
            for (const wchar_t* t : { L"040904b0", L"040904e4", L"000004b0" })
                tables.emplace_back(t);
            for (const std::wstring& t : tables)
            {
                const std::wstring key = L"\\StringFileInfo\\" + t + L"\\ProductVersion";
                wchar_t* value = nullptr;
                UINT len = 0;
                if (VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &len) && value != nullptr && len > 0)
                    if (const std::string v = pf::trimmed(utf8(std::wstring(value, wcsnlen(value, len)))); !v.empty())
                        return v;
            }
            VS_FIXEDFILEINFO* fixed = nullptr;
            UINT fixedLen = 0;
            if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedLen) && fixed != nullptr
                && fixedLen >= sizeof(VS_FIXEDFILEINFO))
            {
                const unsigned d = LOWORD(fixed->dwProductVersionLS);
                return std::to_string(HIWORD(fixed->dwProductVersionMS)) + "." + std::to_string(LOWORD(fixed->dwProductVersionMS))
                       + "." + std::to_string(HIWORD(fixed->dwProductVersionLS)) + (d != 0 ? "." + std::to_string(d) : "");
            }
            return {};
        }

        // The folder a step walks from (root, else home) and the names below it.
        std::pair<fs::path, std::vector<std::string>> walkOf(const Places& places, const fs::path& folder)
        {
            const fs::path n = folder.lexically_normal();
            for (const fs::path& base : { places.root, places.home })
            {
                if (base.empty())
                    continue;
                const fs::path rel = n.lexically_relative(base.lexically_normal());
                if (rel.empty() || *rel.begin() == "..")
                    continue;
                std::vector<std::string> names;
                for (const fs::path& part : rel)
                    if (part != ".")
                        names.push_back(pathText(part));
                return { base, names };
            }
            return { n, {} };
        }

        std::string psList(const std::vector<std::string>& names)
        {
            std::string s = "@(";
            for (std::size_t i = 0; i < names.size(); ++i)
                s += (i == 0 ? "" : ", ") + psQuote(names[i]);
            return s + ")";
        }

        // The script's head: what it is, then its functions (Fp*: PowerShell's aliases, move and copy among them, come
        // before functions of the same name).
        std::string psHead(const std::string& what)
        {
            std::string s;
            s += "# " + what + " for all users: generated for this one run, then run once as an administrator through Windows'\n";
            s += "# own prompt (UAC), its text passed to powershell.exe (-EncodedCommand), never a file. Every path is a literal\n";
            s += "# between single quotes. .NET alone: no cmdlet, so no module is loaded from a folder this user could change.\n";
            s += "# The first failure ends it, its exit code saying which step and why (1000 + 100 * step + reason). Nothing is\n";
            s += "# deleted: what is replaced or removed stays in its staging folder, given to this user, whose installer moves\n";
            s += "# it to the Recycle Bin.\n";
            s += "$ErrorActionPreference = 'Stop'\n";
            s += "$env:PSModulePath = [IO.Path]::Combine($PSHOME, 'Modules')\n";
            s += "$FpSys = [Environment]::SystemDirectory\n";
            s += "$FpRobocopy = [IO.Path]::Combine($FpSys, 'robocopy.exe')\n";
            s += "$FpIcacls = [IO.Path]::Combine($FpSys, 'icacls.exe')\n";
            s += "$FpUser = " + psQuote("*" + thisUser().text) + "\n";
            s += R"PS(function FpFail([int]$k, [int]$why) { exit (1000 + 100 * $k + $why) }
function FpThere([string]$p) { try { [void][IO.File]::GetAttributes($p); return $true } catch { return $false } }
function FpLink([string]$p) { return ([int]([IO.File]::GetAttributes($p) -band [IO.FileAttributes]::ReparsePoint) -ne 0) }
# The folder from $base down $names, each a real folder (no link, no file), made when $make; $null when one is missing
# and not to be made.
function FpFolder([int]$k, [string]$base, [string[]]$names, [bool]$make) {
  if (-not [IO.Directory]::Exists($base)) { FpFail $k 3 }
  $p = $base
  foreach ($n in $names) {
    $p = [IO.Path]::Combine($p, $n)
    if (FpThere $p) {
      if (FpLink $p) { FpFail $k 2 }
      if (-not [IO.Directory]::Exists($p)) { FpFail $k 3 }
    } elseif ($make) {
      try { [void][IO.Directory]::CreateDirectory($p) } catch { FpFail $k 1 }
      if (FpLink $p) { FpFail $k 2 }
    } else {
      return $null
    }
  }
  return $p
}
# A rename to where nothing is: it never replaces.
function FpMove([string]$from, [string]$to) {
  if ([IO.Directory]::Exists($from)) { [IO.Directory]::Move($from, $to) } else { [IO.File]::Move($from, $to) }
}
# The staging folder and what is in it given to this user to read and remove, never to write (it is in a folder
# every user's hosts scan), for the installer to empty into the Recycle Bin.
function FpGive([string]$st) { $null = & $FpIcacls $st /grant ($FpUser + ':(OI)(CI)RX') ($FpUser + ':(OI)(CI)(DE)') /T /C /L /Q }
# A copy that follows no link: a link copied as one, a junction left out.
function FpCopy([string]$src, [string]$st, [string]$n, [bool]$dir) {
  if ($dir) {
    $null = & $FpRobocopy $src ([IO.Path]::Combine($st, $n)) /E /SL /XJ /COPY:DAT /DCOPY:DAT /R:0 /W:0 /NFL /NDL /NJH /NJS /NP
  } else {
    $null = & $FpRobocopy ([IO.Path]::GetDirectoryName($src)) $st ([IO.Path]::GetFileName($src)) /SL /COPY:DAT /R:0 /W:0 /NFL /NDL /NJH /NJS /NP
  }
  return ($LASTEXITCODE -lt 8)
}
function FpStage([int]$k, [string]$f, [string]$sn) {
  $st = [IO.Path]::Combine($f, $sn)
  if (FpThere $st) { FpFail $k 5 }
  try { [void][IO.Directory]::CreateDirectory($st) } catch { FpFail $k 5 }
  if (FpLink $st) { FpFail $k 5 }
  return $st
}
# A part: copied into a staging folder beside its place, then renamed in (the old copy aside first, put back on a
# failure; once the new one is in, the old one takes its own name back there: this user may not rename it).
function FpInstall([int]$k, [string]$base, [string[]]$names, [string]$n, [string]$src, [bool]$dir, [string]$sn) {
  $f = FpFolder $k $base $names $true
  $t = [IO.Path]::Combine($f, $n)
  if ((FpThere $t) -and (FpLink $t)) { FpFail $k 4 }
  $st = FpStage $k $f $sn
  if (-not (FpCopy $src $st $n $dir)) { FpGive $st; FpFail $k 6 }
  $new = [IO.Path]::Combine($st, $n)
  if (FpThere $t) {
    $old = [IO.Path]::Combine($st, 'old')
    try { FpMove $t $old } catch { FpGive $st; FpFail $k 7 }
    try { FpMove $new $t } catch { try { FpMove $old $t } catch { }; FpGive $st; FpFail $k 8 }
    try { FpMove $old $new } catch { }
    FpGive $st
  } else {
    try { FpMove $new $t } catch { FpGive $st; FpFail $k 9 }
    try { [IO.Directory]::Delete($st, $false) } catch { FpGive $st }
  }
}
# A copy: renamed aside into a staging folder beside it.
function FpRemove([int]$k, [string]$base, [string[]]$names, [string]$n, [string]$sn) {
  $f = FpFolder $k $base $names $false
  if ($null -eq $f) { return }
  $t = [IO.Path]::Combine($f, $n)
  if (-not (FpThere $t)) { return }
  if (FpLink $t) { FpFail $k 4 }
  $st = FpStage $k $f $sn
  try { FpMove $t ([IO.Path]::Combine($st, $n)) } catch { try { [IO.Directory]::Delete($st, $false) } catch { }; FpFail $k 10 }
  FpGive $st
}
)PS";
            return s;
        }

        // A step's failure, as the script's exit code gave it, in words.
        std::string reasonWords(int reason, const Engine& engine, const fs::path& target)
        {
            const std::string f = engine.shown(target.parent_path()), t = engine.shown(target);
            switch (reason)
            {
                case 1:  return "it could not make the folder " + f;
                case 2:  return "a folder on the way to " + f
                                + " is a link: this installer writes only into real folders, and left it as it is";
                case 3:  return "a file is where a folder on the way to " + f + " should be";
                case 4:  return t + " is a link: left as it is";
                case 5:  return "it may not write in " + f;
                case 6:  return "it could not copy it into " + f;
                case 7:  return "it could not move the old copy aside";
                case 8:  return "it could not put the new copy in place";
                case 9:  return "it could not put it in " + f;
                case 10: return std::string("it could not move it aside, on its way to ") + trashWords();
                default: break;
            }
            return "it stopped, as an administrator (reason " + std::to_string(reason) + ")";
        }
    }

    // ---- EnginePlatform.h --------------------------------------------------------------------------------------------

    pf::Node pf::nodeOf(const fs::path& p)
    {
        Node n;
        WIN32_FILE_ATTRIBUTE_DATA d{};
        // The entry's own attributes (a link's, not what it leads to).
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        {
            n.error = static_cast<int>(GetLastError());
            n.missing = n.error == ERROR_FILE_NOT_FOUND || n.error == ERROR_PATH_NOT_FOUND;
            return n;
        }
        n.exists = true;
        const DWORD a = d.dwFileAttributes;
        if ((a & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            // A symbolic link or a junction is a link; another reparse point (a cloud file, deduplication) is what it
            // stands for. One whose tag cannot be read counts as a link.
            WIN32_FIND_DATAW fd{};
            const HANDLE h = FindFirstFileExW(p.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
            if (h != INVALID_HANDLE_VALUE)
            {
                FindClose(h);
                n.link = fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
            }
            else
                n.link = true;
        }
        n.dir = !n.link && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        n.file = !n.link && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!n.link && GetNamedSecurityInfoW(p.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
                                             nullptr, &sd) == ERROR_SUCCESS)
        {
            const UserSid& me = thisUser();
            n.mine = owner != nullptr && !me.sid.empty() && EqualSid(owner, const_cast<BYTE*>(me.sid.data())) != FALSE;
            if (!n.mine && owner != nullptr)
                n.owner = accountName(owner);
            LocalFree(sd);
        }
        return n;
    }

    bool pf::writable(const fs::path& dir)
    {
        // The system's own access check: may this user add a file and a folder here.
        const HANDLE h = CreateFileW(dir.c_str(), FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        CloseHandle(h);
        return true;
    }

    bool pf::mayMove(const fs::path& path)
    {
        // Renaming it away takes DELETE on it (or the folder's DELETE_CHILD), and adding where it goes. A copy in use
        // (a sharing violation) is no question of permission: the move itself will say.
        bool may = false;
        const HANDLE h = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
            may = true;
        }
        else if (GetLastError() == ERROR_SHARING_VIOLATION)
            may = true;
        else
        {
            const HANDLE parent = CreateFileW(path.parent_path().c_str(), FILE_DELETE_CHILD,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                              FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (parent != INVALID_HANDLE_VALUE)
            {
                CloseHandle(parent);
                may = true;
            }
        }
        return may && writable(path.parent_path());
    }

    std::string pf::linkTarget(const fs::path& link)
    {
        // Opened through the link: where it leads, as Windows names it.
        const HANDLE h = CreateFileW(link.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return "somewhere it cannot read";
        std::wstring buf(32768, L'\0');
        const DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(h);
        if (n == 0 || n >= buf.size())
            return "somewhere it cannot read";
        buf.resize(n);
        if (buf.rfind(L"\\\\?\\UNC\\", 0) == 0)
            buf = L"\\\\" + buf.substr(8);
        else if (buf.rfind(L"\\\\?\\", 0) == 0)
            buf = buf.substr(4);
        return utf8(buf);
    }

    std::string pf::errorText(int error)
    {
        wchar_t* text = nullptr;
        const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
        std::string s = n > 0 && text != nullptr ? utf8(std::wstring(text, n)) : "error " + std::to_string(error);
        LocalFree(text);
        s = pf::trimmed(s);
        while (!s.empty() && (s.back() == '.' || s.back() == '\r' || s.back() == '\n'))
            s.pop_back();                                // within a sentence, as strerror's words are
        return s;
    }

    bool pf::renameItem(const fs::path& from, const fs::path& to, int& error)
    {
        if (MoveFileExW(from.c_str(), to.c_str(), 0))
            return true;
        error = static_cast<int>(GetLastError());
        return false;
    }

    bool pf::renameNew(const fs::path& from, const fs::path& to, int& error)
    {
        return renameItem(from, to, error);              // MoveFileEx never replaces without MOVEFILE_REPLACE_EXISTING
    }

    bool pf::swapItems(const fs::path& a, const fs::path& b, int& error)
    {
        // Three renames: b aside (beside a: in the staging folder), a into b's place (b put back if that fails), and
        // b's old self where a was. Between the first two, nothing is at b: a moment without the part, never a broken
        // one. b's folder must take a new entry first: the second rename and putting b back both need it, so a folder
        // this user may no longer add to fails here, before anything moved.
        if (!writable(b.parent_path()))
        {
            error = ERROR_ACCESS_DENIED;
            return false;
        }
        const fs::path aside = a.parent_path() / (a.filename().wstring() + L".swap");
        if (!MoveFileExW(b.c_str(), aside.c_str(), 0))
        {
            error = static_cast<int>(GetLastError());
            return false;
        }
        if (!MoveFileExW(a.c_str(), b.c_str(), 0))
        {
            error = static_cast<int>(GetLastError());
            MoveFileExW(aside.c_str(), b.c_str(), 0);
            return false;
        }
        MoveFileExW(aside.c_str(), a.c_str(), 0);        // a name just freed: the old copy where the new one was
        return true;
    }

    bool pf::removeEmptyFolder(const fs::path& p)
    {
        return RemoveDirectoryW(p.c_str()) != FALSE;
    }

    fs::path pf::makeStage(const fs::path& folder, int& error)
    {
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const fs::path p = folder / (".installing-" + randomText(6));
            if (CreateDirectoryW(p.c_str(), nullptr))
                return p;
            error = static_cast<int>(GetLastError());
            if (error != ERROR_ALREADY_EXISTS)
                return {};
        }
        return {};
    }

    std::string pf::cdhash(const fs::path&)
    {
        return {};                                       // no code signature compared here: the bytes are
    }

    std::string pf::now()
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char b[32];
        std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
        return b;
    }

    std::string pf::appFile(const std::string& name)
    {
        return name + ".exe";
    }

    bool pf::sameName(const fs::path& a, const fs::path& b)
    {
        const std::wstring x = a.wstring(), y = b.wstring();
        return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE)
               == CSTR_EQUAL;
    }

    std::string pf::hostName(const fs::path& app)
    {
        const std::string stem = lowerAscii(pathText(app.stem()));
        for (const Host& h : kHosts)
        {
            const std::string exe = lowerAscii(h.exe);
            if (stem.size() < exe.size() || stem.compare(0, exe.size(), exe) != 0)
                continue;
            const char next = stem.size() == exe.size() ? '\0' : stem[exe.size()];
            if (next == '\0' || next == ' ' || next == '.' || next == '-' || (next >= '0' && next <= '9'))
                return h.name;
        }
        return {};
    }

    // ---- places, folders and versions --------------------------------------------------------------------------------

    Places Places::thisMachine()
    {
        Places p;
        p.home = knownFolder(FOLDERID_LocalAppData);
        p.root = knownFolder(FOLDERID_ProgramFiles);
        return p;
    }

    Places Places::thisMachine(const std::string& logName)
    {
        Places p = thisMachine();
        if (!p.home.empty() && pf::plainName(logName))
            p.log = p.home / logName / (logName + ".log");
        return p;
    }

    std::string Engine::bundleVersion(const fs::path& bundle)
    {
        // A VST3: the version its moduleinfo.json says (JUCE writes one), else its binary's.
        const fs::path info = bundle / "Contents" / "Resources" / "moduleinfo.json";
        if (pf::nodeOf(info).file)
        {
            const juce::var v = juce::JSON::parse(juce::String::fromUTF8(pf::fileText(info).c_str()));
            if (const std::string version = v.getProperty("Version", {}).toString().toStdString(); !version.empty())
                return version;
        }
        for (const fs::path& pe : { bundle / "Contents" / "x86_64-win" / (bundle.stem().wstring() + L".vst3"),
                                    bundle / (bundle.filename().wstring() + L".exe") })
            if (pf::nodeOf(pe).file)
                return peVersion(pe);
        return {};
    }

    fs::path Engine::folder(const Product& product, Part part, Scope scope) const
    {
        const bool user = scope == Scope::user;
        switch (part)
        {
            case Part::au:        break;                 // none on Windows
            case Part::vst3:      return user ? places_.home / "Programs" / "Common" / "VST3" : places_.root / "Common Files" / "VST3";
            case Part::app:
            case Part::extension: return user ? places_.home / "Programs" / product.company : places_.root / product.company;
        }
        return {};
    }

    std::vector<std::pair<fs::path, Scope>> Engine::knownFolders(const Product& product, Part part) const
    {
        return { { folder(product, part, Scope::user), Scope::user }, { folder(product, part, Scope::system), Scope::system } };
    }

    std::string Engine::shown(const fs::path& p) const
    {
        const fs::path n = p.lexically_normal();
        const auto below = [&](const fs::path& base, fs::path& rel) {
            rel = n.lexically_relative(base.lexically_normal());
            return !rel.empty() && *rel.begin() != "..";
        };
        fs::path rel;
        if (!places_.home.empty() && below(places_.home, rel))
            return rel == "." ? std::string("%LOCALAPPDATA%") : "%LOCALAPPDATA%\\" + pathText(rel);
        if (!places_.root.empty() && below(places_.root, rel))
            return rel == "." ? std::string("%ProgramFiles%") : "%ProgramFiles%\\" + pathText(rel);
        return pathText(n);
    }

    // ---- "All users": the script, run as an administrator --------------------------------------------------------------

    std::string Engine::adminScript(const Plan& plan, const fs::path& statusDir) const
    {
        const std::string run = statusDir.filename().string();
        std::string s = psHead("Installing");
        for (std::size_t i = 0; i < plan.steps.size(); ++i)
        {
            const Step& st = plan.steps[i];
            if (!st.changes())
                continue;
            const int k = static_cast<int>(i);
            const auto [base, names] = walkOf(places_, st.folder);
            s += "\n# " + std::to_string(k) + ": " + st.product + ", " + partName(st.part) + " (" + pf::actionWords(st.action) + ")\n";
            s += "FpInstall " + std::to_string(k) + " " + psQuote(pathText(base)) + " " + psList(names) + " "
                 + psQuote(pathText(st.target.filename())) + " " + psQuote(pathText(st.source)) + " "
                 + (st.part == Part::extension ? "$false" : "$true") + " " + psQuote(stageName(run, k)) + "\n";
        }
        s += "\nexit 0\n";
        return s;
    }

    std::string Engine::adminUninstallScript(const std::vector<Copy>& copies, const fs::path& statusDir,
                                             const std::vector<int>& only) const
    {
        const std::string run = statusDir.filename().string();
        std::string s = psHead("Uninstalling");
        for (std::size_t i = 0; i < copies.size(); ++i)
        {
            const Copy& c = copies[i];
            if (c.link || (!only.empty() && std::find(only.begin(), only.end(), static_cast<int>(i)) == only.end()))
                continue;                                // links stay; the copies not asked for are not its
            const int k = static_cast<int>(i);
            const auto [base, names] = walkOf(places_, c.path.parent_path());
            s += "\n# " + std::to_string(k) + ": " + pathText(c.path.filename()) + "\n";
            s += "FpRemove " + std::to_string(k) + " " + psQuote(pathText(base)) + " " + psList(names) + " "
                 + psQuote(pathText(c.path.filename())) + " " + psQuote(stageName(run, k)) + "\n";
        }
        s += "\nexit 0\n";
        return s;
    }

    std::string Engine::collectStage(const fs::path& stage, const fs::path& target, bool replaced)
    {
        const std::string name = target.filename().string();
        std::vector<fs::path> inside;
        bool ours = true;
        std::error_code ec;
        for (fs::directory_iterator it(stage, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string e = it->path().filename().string();
            ours = ours && (e == name || e == "old") && !pf::nodeOf(it->path()).link;
            inside.push_back(it->path());
        }
        if (!ours || ec)
            return "what the run left stays in " + shown(stage);
        std::string said;
        for (const fs::path& item : inside)
        {
            // The script gave the old copy its own name back once the new one was in place; `old` when it could not
            // (this user may not rename it there: it goes to the Recycle Bin as `old`).
            const bool old = item.filename() == "old" || replaced;
            std::string where, error;
            if (trash(item, where, error))
                said = (old ? std::string("the old copy") : shown(target)) + " is in " + where;
            else
                said = (old ? std::string("the old copy") : std::string("the copy")) + " stays in " + shown(stage) + " ("
                       + error + ")";
        }
        pf::removeEmptyFolder(stage);
        return said;
    }

    Report Engine::runAdmin(const std::function<std::string(const fs::path&)>& makeScript, int steps,
                            const std::vector<int>& indices, const ProgressFn& progress, Report report,
                            const std::string& what, const std::vector<fs::path>& targets)
    {
        Report r = std::move(report);
        const auto setState = [&](int i, State s, const std::string& line) {
            if (i < 0 || i >= steps)
                return;
            r.states[static_cast<std::size_t>(i)] = s;
            r.lines[static_cast<std::size_t>(i)] = line;
            if (progress)
                progress(i, s, line);
        };
        const auto targetOf = [&](int i) {
            return i >= 0 && static_cast<std::size_t>(i) < targets.size() ? targets[static_cast<std::size_t>(i)] : fs::path();
        };

        // The run's name: its staging folders are named from it (stageName), in the script and here. No status folder:
        // the script's report is its exit code.
        const fs::path run = "installer-" + randomText(12);
        const std::string script = makeScript(run);
        for (const int i : indices)
            setState(i, State::waiting, "waiting for an administrator to approve it");
        std::string error;
        int status = -1;
        const System::Admin result = system_.runAsAdministrator(
            script, payload_.project + " Installer wants to " + (what == "install" ? "install" : "uninstall")
                        + " plugins for all users.",
            error, status);
        if (result == System::Admin::cancelled)
        {
            r.cancelled = true;
            for (const int i : indices)
                setState(i, State::notRun, std::string("not changed: ") + pf::kNoAdmin);
            log(r, "the administrator prompt was cancelled: nothing ran");
            return r;
        }

        // 0: every step done. 1000 + 100 * step + reason: the steps before it done, it failed, the rest not run.
        // Anything else: nothing is known to be done.
        const bool all = result == System::Admin::ran && status == 0;
        int failedAt = -1, reason = 0;
        if (!all && status >= 1000)
        {
            failedAt = (status - 1000) / 100;
            reason = (status - 1000) % 100;
            if (std::find(indices.begin(), indices.end(), failedAt) == indices.end())
                failedAt = -1;
        }
        bool before = true;
        for (const int i : indices)
        {
            if (all || (failedAt >= 0 && before && i != failedAt))
                setState(i, State::done, std::string());
            else if (i == failedAt)
            {
                setState(i, State::failed, reasonWords(reason, *this, targetOf(i)));
                before = false;
            }
            else
                setState(i, State::notRun, "not changed");
        }
        if (!all && failedAt < 0)
            r.error = "Windows could not run the " + what + " as an administrator ("
                      + (pf::trimmed(error).empty() ? "PowerShell stopped with status " + std::to_string(status) : pf::trimmed(error))
                      + ").";

        // What each step left in its staging folder (the old copy, the copy moved aside, a half-made copy), as this
        // user: to the Recycle Bin, the folder removed.
        for (const int i : indices)
        {
            const fs::path target = targetOf(i);
            if (target.empty())
                continue;
            const fs::path stage = target.parent_path() / stageName(run.filename().string(), i);
            if (!pf::nodeOf(stage).exists)
                continue;
            const std::string said = collectStage(stage, target,
                                                  what == "install" && r.states[static_cast<std::size_t>(i)] == State::done);
            if (r.states[static_cast<std::size_t>(i)] == State::done && !said.empty())
                setState(i, State::done, said);
            else if (!said.empty())
                log(r, said);
        }
        for (const int i : indices)
            log(r, "step " + std::to_string(i) + ": "
                   + (r.states[static_cast<std::size_t>(i)] == State::done ? "done" : "not done") + " "
                   + r.lines[static_cast<std::size_t>(i)]);
        return r;
    }
}
