#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hlaftp::checker {
inline std::wstring Utf8ToWide(std::string_view input) {
    if (input.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (!length) throw std::runtime_error("Invalid UTF-8 in OpenVR configuration");
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(), length))
        throw std::runtime_error("Cannot decode OpenVR configuration");
    return result;
}

// A bounded JSON reader: validates the whole document, including unknown fields.
// The only extracted setting is the first runtime installation path.
class RuntimeJson {
public:
    explicit RuntimeJson(std::string_view text) : text_(text) {}
    std::filesystem::path Parse() {
        // OpenVR normally emits UTF-8 without BOM; accept a UTF-8 BOM as well.
        if (text_.substr(0, 3) == "\xef\xbb\xbf") pos_ = 3;
        Expect('{');
        bool found = false;
        std::wstring runtime;
        if (!Take('}')) {
            do {
                const auto name = String();
                Expect(':');
                if (name == L"runtime") {
                    if (found) Fail("Duplicate runtime setting");
                    found = true;
                    Expect('[');
                    if (Take(']')) Fail("OpenVR runtime array is empty");
                    runtime = String();
                    while (Take(',')) { (void)String(); }
                    Expect(']');
                } else {
                    Value(1);
                }
            } while (Take(','));
            Expect('}');
        }
        Space();
        if (pos_ != text_.size()) Fail("Trailing data after OpenVR configuration");
        if (!found || runtime.empty()) Fail("OpenVR runtime[0] is missing or empty");
        if (runtime.find(L'\0') != std::wstring::npos) Fail("OpenVR runtime path contains a null character");
        std::filesystem::path path(runtime);
        if (!path.is_absolute()) Fail("OpenVR runtime[0] must be an absolute path");
        return path / L"bin" / L"vrclient_x64.dll";
    }
private:
    std::string_view text_;
    size_t pos_ = 0;
    [[noreturn]] static void Fail(const char* message = "Malformed OpenVR configuration JSON") { throw std::runtime_error(message); }
    void Space() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_; }
    bool Take(char ch) { Space(); if (pos_ < text_.size() && text_[pos_] == ch) { ++pos_; return true; } return false; }
    void Expect(char ch) { if (!Take(ch)) Fail(); }
    unsigned Hex4() {
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ == text_.size()) Fail();
            const char c = text_[pos_++];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else Fail();
            result = result * 16 + digit;
        }
        return result;
    }
    std::wstring String() {
        Expect('"');
        std::wstring result;
        while (pos_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return result;
            if (c < 0x20) Fail();
            if (c != '\\') {
                const size_t start = pos_ - 1;
                while (pos_ < text_.size() && static_cast<unsigned char>(text_[pos_]) >= 0x20 && text_[pos_] != '"' && text_[pos_] != '\\') ++pos_;
                result += Utf8ToWide(text_.substr(start, pos_ - start));
                continue;
            }
            if (pos_ == text_.size()) Fail();
            switch (text_[pos_++]) {
            case '"': result += L'"'; break;
            case '\\': result += L'\\'; break;
            case '/': result += L'/'; break;
            case 'b': result += L'\b'; break;
            case 'f': result += L'\f'; break;
            case 'n': result += L'\n'; break;
            case 'r': result += L'\r'; break;
            case 't': result += L'\t'; break;
            case 'u': {
                const unsigned first = Hex4();
                if (first >= 0xdc00 && first <= 0xdfff) Fail();
                result += static_cast<wchar_t>(first);
                if (first >= 0xd800 && first <= 0xdbff) {
                    if (pos_ + 2 > text_.size() || text_.substr(pos_, 2) != "\\u") Fail();
                    pos_ += 2;
                    const unsigned second = Hex4();
                    if (second < 0xdc00 || second > 0xdfff) Fail();
                    result += static_cast<wchar_t>(second);
                }
                break;
            }
            default: Fail();
            }
        }
        Fail();
    }
    bool Digit() const { return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9'; }
    void Digits() { if (!Digit()) Fail(); while (Digit()) ++pos_; }
    void Number() {
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '0') ++pos_;
        else Digits();
        if (pos_ < text_.size() && text_[pos_] == '.') { ++pos_; Digits(); }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            Digits();
        }
    }
    void Literal(std::string_view value) { if (text_.substr(pos_, value.size()) != value) Fail(); pos_ += value.size(); }
    void Value(unsigned depth) {
        if (depth > 64) Fail("OpenVR configuration nesting exceeds limit");
        Space();
        if (pos_ == text_.size()) Fail();
        switch (text_[pos_]) {
        case '"': (void)String(); break;
        case '{':
            ++pos_;
            if (!Take('}')) {
                do { (void)String(); Expect(':'); Value(depth + 1); } while (Take(','));
                Expect('}');
            }
            break;
        case '[':
            ++pos_;
            if (!Take(']')) { do { Value(depth + 1); } while (Take(',')); Expect(']'); }
            break;
        case 't': Literal("true"); break;
        case 'f': Literal("false"); break;
        case 'n': Literal("null"); break;
        default: Number();
        }
    }
};
}
