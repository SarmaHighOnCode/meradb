// cpp/ftxui_patch/ftxui_win_input.h -- joins the UTF-16 halves of a character outside the BMP (an emoji).
//
// The Windows console delivers a typed or pasted character as UTF-16 units, one key event each. FTXUI 5 converts every unit on
// its own, so the two halves of a surrogate pair were dropped or turned into invalid bytes. cmake/patch_ftxui.cmake makes
// FTXUI's Windows input loop pass each unit through SurrogateJoiner instead; the unit tests include this header directly.
// Header only, no dependencies.
#pragma once
#include <cstdint>
#include <string>

namespace ftxui_patch {

class SurrogateJoiner {
public:
    // Which console key events carry text. FTXUI skips every key-up. But the pseudo console (Windows Terminal, VS Code, ...)
    // delivers a character outside the BMP as the keystrokes of an Alt+numpad sequence: the two halves arrive on the
    // key-up of the Alt key (virtual key 0x12), between key-downs without a character. Those key-ups must be read.
    static bool wantsKeyEvent(bool keyDown, std::uint16_t virtualKey, std::uint16_t unit) {
        return keyDown || (virtualKey == 0x12 && (isHigh(unit) || isLow(unit)));
    }

    // Feeds one UTF-16 unit and returns the UTF-8 bytes that are ready ("" while a high half waits for its low half, and
    // for an orphan half, which is dropped). Unit 0 gives one NUL byte, as FTXUI always passed it on; it does not
    // disturb a high half that is waiting (the Alt key-downs of the sequence above carry no character).
    std::string push(std::uint16_t unit) {
        if (unit == 0) return std::string(1, '\0');
        if (isHigh(unit)) {
            high_ = unit;   // a second high half replaces the first, which had no partner
            return std::string();
        }
        if (isLow(unit)) {
            const std::uint16_t high = high_;
            high_ = 0;
            if (high == 0) return std::string();   // orphan low half
            const std::uint32_t cp = 0x10000u + ((static_cast<std::uint32_t>(high) - 0xD800u) << 10) +
                                     (static_cast<std::uint32_t>(unit) - 0xDC00u);
            return utf8(cp);
        }
        high_ = 0;   // a high half followed by an ordinary unit: the half is dropped
        return utf8(unit);
    }
    bool pending() const { return high_ != 0; }
    void reset() { high_ = 0; }

private:
    static bool isHigh(std::uint16_t u) { return u >= 0xD800 && u <= 0xDBFF; }
    static bool isLow(std::uint16_t u) { return u >= 0xDC00 && u <= 0xDFFF; }
    static std::string utf8(std::uint32_t cp) {
        std::string out;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return out;
    }
    std::uint16_t high_ = 0;
};

}  // namespace ftxui_patch
