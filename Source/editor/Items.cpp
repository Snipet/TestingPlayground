// Source/editor/Items.cpp: the text helpers Items.h declares.
#include "editor/Items.h"

#include <cstdint>

namespace playground::ui
{
    std::string asciiText(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size();)
        {
            const auto c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)
            {
                if (c == '\t' || c == '\n' || (c >= 32 && c < 127))
                    out += static_cast<char>(c);
                ++i;
                continue;
            }
            // One UTF-8 sequence: its codepoint, or '?' for a malformed one.
            const int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            uint32_t cp = n == 4 ? c & 0x07u : n == 3 ? c & 0x0Fu : c & 0x1Fu;
            bool ok = n > 1 && i + static_cast<std::size_t>(n) <= s.size();
            for (int k = 1; ok && k < n; ++k)
            {
                const auto cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
                ok = (cc & 0xC0u) == 0x80u;
                cp = (cp << 6) | (cc & 0x3Fu);
            }
            i += ok ? static_cast<std::size_t>(n) : 1;
            if (!ok)
            {
                out += '?';
                continue;
            }
            switch (cp)
            {
                case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: out += ' '; break;
                case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: out += '-'; break;
                case 0x2014: case 0x2015: out += "--"; break;
                case 0x2018: case 0x2019: case 0x201B: case 0x2032: out += '\''; break;
                case 0x201C: case 0x201D: case 0x201F: case 0x2033: out += '"'; break;
                case 0x2026: out += "..."; break;
                case 0x2022: case 0x2023: case 0x25CF: case 0x00B7: out += '*'; break;
                case 0x00D7: out += 'x'; break;
                case 0x00F7: out += '/'; break;
                case 0x2192: case 0x27F6: out += "->"; break;
                case 0x2190: out += "<-"; break;
                case 0x2194: out += "<->"; break;
                case 0x21D2: out += "=>"; break;
                case 0x2264: out += "<="; break;
                case 0x2265: out += ">="; break;
                case 0x2260: out += "!="; break;
                case 0x2248: out += "~="; break;
                case 0x00B1: out += "+-"; break;
                case 0x00B0: out += " deg"; break;
                case 0x00B5: case 0x03BC: out += 'u'; break;
                case 0x03C0: out += "pi"; break;
                case 0x221E: out += "inf"; break;
                case 0x2713: case 0x2714: out += "ok"; break;
                case 0x2717: case 0x2718: out += 'x'; break;
                case 0x00BD: out += "1/2"; break;
                case 0x00BC: out += "1/4"; break;
                case 0x00B2: out += "^2"; break;
                case 0x00B3: out += "^3"; break;
                case 0x221A: out += "sqrt"; break;
                case 0x2211: out += "sum"; break;
                case 0x0394: out += "delta"; break;
                case 0x03C9: out += "w"; break;
                case 0xFE0F: case 0x200B: case 0x200D: break;   // variation selectors, zero widths
                default: out += '?'; break;
            }
        }
        return out;
    }
}
