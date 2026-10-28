#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "log.h"
#include <cstdio>
#include <string>

namespace hlaftp {
namespace {
SRWLOCK logLock = SRWLOCK_INIT;
bool initialized = false;
std::wstring logPath;
std::string Utf8(const wchar_t* value) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}
void Write(std::string_view line, DWORD mode) {
    HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, mode, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}
void Line(std::string_view line, DWORD mode = OPEN_ALWAYS) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char prefix[64]{};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u ", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    std::string message = prefix + std::string(line) + "\r\n";
    OutputDebugStringA(message.c_str());
    Write(message, mode);
}
}
void LogLine(std::string_view line) noexcept {
    AcquireSRWLockExclusive(&logLock);
    try {
        if (!initialized) {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&LogLine), &self);
            wchar_t path[32768]{};
            const auto length = GetModuleFileNameW(self, path, 32768);
            if (!length || length >= 32768) throw 0;
            logPath.assign(path, length);
            logPath.resize(logPath.find_last_of(L"\\/") + 1);
            logPath += L"hla_ftp.log";
            Line("HLA FTP Patch 1.0.0", CREATE_ALWAYS);
            GetModuleFileNameW(nullptr, path, 32768);
            Line("exe=" + Utf8(path));
            initialized = true;
        }
        Line(line);
    } catch (...) {
        OutputDebugStringA("HLA FTP Patch: logging failed\n");
    }
    ReleaseSRWLockExclusive(&logLock);
}
}
