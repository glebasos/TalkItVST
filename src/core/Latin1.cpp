#include "Latin1.h"

namespace talkit {
namespace {

// Decodes one UTF-8 scalar at `i`, advancing it. Returns U+FFFD on bad input
// and still advances by one byte, so malformed text cannot hang the caller.
char32_t decode(std::string_view s, size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) -> bool {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    auto tail = [&](size_t k) { return static_cast<unsigned char>(s[i + k]) & 0x3F; };

    if (b0 < 0x80) { ++i; return b0; }
    if ((b0 & 0xE0) == 0xC0 && cont(1)) {
        char32_t c = ((b0 & 0x1Fu) << 6) | tail(1);
        i += 2;
        return c < 0x80 ? 0xFFFD : c;  // overlong
    }
    if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        char32_t c = ((b0 & 0x0Fu) << 12) | (tail(1) << 6) | tail(2);
        i += 3;
        return c < 0x800 ? 0xFFFD : c;
    }
    if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        char32_t c = ((b0 & 0x07u) << 18) | (tail(1) << 12) | (tail(2) << 6) | tail(3);
        i += 4;
        return c < 0x10000 || c > 0x10FFFF ? 0xFFFD : c;
    }
    ++i;
    return 0xFFFD;
}

}  // namespace

std::string utf8ToLatin1(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        const char32_t c = decode(utf8, i);
        switch (c) {
            case 0x2018: case 0x2019: case 0x201B: out += '\''; continue;  // ' '
            case 0x201C: case 0x201D: case 0x201F: out += '"';  continue;  // " "
            case 0x2013: case 0x2014: case 0x2212: out += '-';  continue;  // - - -
            case 0x2026: out += "..."; continue;                            // ...
            case 0x00A0: case 0x2007: case 0x202F: out += ' '; continue;    // nbsp
            case 0x2022: case 0x00B7: out += ','; continue;                 // bullet -> pause
            default: break;
        }
        if (c <= 0xFF) {
            out += static_cast<char>(static_cast<unsigned char>(c));
        } else {
            out += '?';
        }
    }
    return out;
}

}  // namespace talkit
