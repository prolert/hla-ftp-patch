#include "patcher.h"
#include "pe_image.h"
#include "runtime_path.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace {
constexpr size_t MaxDllBytes = 256 * 1024 * 1024;
constexpr size_t MaxConfigBytes = 1024 * 1024;

std::string Utf8(std::wstring_view input) {
    if (input.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Cannot encode file path");
    std::string output(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), count, nullptr, nullptr))
        throw std::runtime_error("Cannot encode file path");
    return output;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path, size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open file: " + Utf8(path.native()));
    const auto end = file.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > limit) throw std::runtime_error("File exceeds permitted size: " + Utf8(path.native()));
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read complete file: " + Utf8(path.native()));
    return bytes;
}

std::filesystem::path DefaultRuntime() {
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!count) throw std::runtime_error("LOCALAPPDATA is missing; pass the vrclient_x64.dll path explicitly");
    std::wstring value(count, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), count);
    if (!copied || copied >= count) throw std::runtime_error("Cannot read LOCALAPPDATA");
    value.resize(copied);
    const auto config = std::filesystem::path(value) / L"openvr" / L"openvrpaths.vrpath";
    std::cout << "Configuration: " << Utf8(config.native()) << '\n';
    const auto bytes = ReadFile(config, MaxConfigBytes);
    const std::string text(bytes.begin(), bytes.end());
    return hlaftp::checker::RuntimeJson(text).Parse();
}

std::string HexBytes(const std::vector<uint8_t>& bytes) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) stream << ' ';
        stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return stream.str();
}

void Usage() {
    std::cout << "Usage: ftp_check.exe [path-to-vrclient_x64.dll]\n"
                 "Without a path, reads LOCALAPPDATA/openvr/openvrpaths.vrpath runtime[0].\n"
                 "Checks files read-only; no patch is written.\n";
}
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && (std::wstring_view(argv[1]) == L"--help" || std::wstring_view(argv[1]) == L"-h")) { Usage(); return 0; }
    if (argc > 2 || (argc == 2 && (std::wstring_view(argv[1]).empty() || std::wstring_view(argv[1]).starts_with(L"--")))) { Usage(); return 1; }
    try {
        const auto path = argc == 2 ? std::filesystem::path(argv[1]) : DefaultRuntime();
        std::cout << "File: " << Utf8(path.native()) << '\n';
        const auto bytes = ReadFile(path, MaxDllBytes);
        const auto image = hlaftp::MapFileImage(bytes);
        const auto result = hlaftp::FindPatchSite(image.data(), image.size());
        const bool success = result.status == hlaftp::PatchStatus::Found || result.status == hlaftp::PatchStatus::AlreadyPatched || result.status == hlaftp::PatchStatus::NotNeeded;
        std::cout << (success ? "OK: " : "Error: ") << hlaftp::StatusName(result.status);
        switch (result.status) {
        case hlaftp::PatchStatus::Found: std::cout << " - the patch can be applied"; break;
        case hlaftp::PatchStatus::AlreadyPatched: std::cout << " - this image is already patched"; break;
        case hlaftp::PatchStatus::NotNeeded: std::cout << " - validation anchor is absent; no patch is needed for known older builds"; break;
        case hlaftp::PatchStatus::PatternChanged: std::cout << " - runtime code changed; an updated patch may be required"; break;
        default: break;
        }
        std::cout << '\n';
        if (result.site) {
            std::cout << "RVA: 0x" << std::hex << std::uppercase << result.site->rva << std::dec << '\n'
                      << "Before: " << HexBytes(result.site->before) << '\n'
                      << "After:  " << HexBytes(result.site->after) << '\n';
        }
        if (!result.detail.empty()) std::cout << "Detail: " << result.detail << '\n';
        return success ? 0 : 1;
    } catch (const std::exception& error) {
        std::cout << "Error: " << error.what() << '\n';
        return 1;
    }
}
