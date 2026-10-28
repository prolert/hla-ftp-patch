#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "log.h"
#include "patcher.h"
#include "pe_image.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {
INIT_ONCE originalOnce = INIT_ONCE_STATIC_INIT;
HMODULE original = nullptr;
SRWLOCK patchLock = SRWLOCK_INIT;
HMODULE inspectedModule = nullptr;
std::optional<hlaftp::PatchSite> previousSite;

bool IsAlyx() noexcept {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return false;
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') name = p + 1;
    return CompareStringOrdinal(name, -1, L"hlvr.exe", -1, TRUE) == CSTR_EQUAL;
}

BOOL CALLBACK LoadOriginal(PINIT_ONCE, PVOID, PVOID*) noexcept {
    try {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LoadOriginal), &self)) return FALSE;
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(self, path, 32768);
        if (!length || length >= 32768) return FALSE;
        std::wstring full(path, length);
        full.resize(full.find_last_of(L"\\/") + 1);
        full += L"openvr_api_ftp_original.dll";
        original = LoadLibraryExW(full.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        return original != nullptr;
    } catch (...) { return FALSE; }
}

template<class T> T Resolve(const char* name) noexcept {
    if (!InitOnceExecuteOnce(&originalOnce, LoadOriginal, nullptr, nullptr)) {
        if (IsAlyx()) hlaftp::LogLine("Error: cannot load adjacent openvr_api_ftp_original.dll; reinstall or verify Alyx files.");
        return nullptr;
    }
    return reinterpret_cast<T>(GetProcAddress(original, name));
}

std::string Hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte) << ' ';
    return out.str();
}

void InspectClientCpp(HMODULE client) {
    // Called by public OpenVR entry points outside DllMain/loader callbacks.
    // The exported shutdown wrapper invalidates this cache before a reload.
    auto* base = reinterpret_cast<uint8_t*>(client);
    if (client == inspectedModule) {
        if (!previousSite) return;
        if (std::equal(previousSite->after.begin(), previousSite->after.end(), base + previousSite->rva)) return;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(base, &memory, sizeof(memory))) { hlaftp::LogLine("Error: VirtualQuery failed"); return; }
    // The OS has validated a loaded module's headers. Still bound the NT header access.
    if (memory.RegionSize < sizeof(IMAGE_NT_HEADERS64)) { hlaftp::LogLine("Error: module header is truncated"); return; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) > memory.RegionSize - sizeof(IMAGE_NT_HEADERS64)) {
        hlaftp::LogLine("Error: module DOS header is invalid"); return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    const auto result = hlaftp::ApplyPatch(base, size);
    inspectedModule = client;
    previousSite = result.site;
    wchar_t modulePath[32768]{};
    GetModuleFileNameW(client, modulePath, 32768);
    const int count = WideCharToMultiByte(CP_UTF8, 0, modulePath, -1, nullptr, 0, nullptr, nullptr);
    std::string path(count > 0 ? static_cast<size_t>(count) : 1, '\0');
    if (count > 0) WideCharToMultiByte(CP_UTF8, 0, modulePath, -1, path.data(), count, nullptr, nullptr);
    path.pop_back();
    std::ostringstream message;
    message << hlaftp::StatusName(result.status) << " module=" << path << " TimeDateStamp=0x" << std::hex
        << nt->FileHeader.TimeDateStamp << " SizeOfImage=0x" << size;
    if (result.site) message << " RVA=0x" << result.site->rva << " before=[" << Hex(result.site->before)
        << "] after=[" << Hex(result.site->after) << ']';
    message << " detail=" << result.detail;
    hlaftp::LogLine(message.str());
}

void InspectClientCaught(HMODULE client) noexcept {
    try { InspectClientCpp(client); }
    catch (const std::exception& error) { hlaftp::LogLine(error.what()); }
    catch (...) { hlaftp::LogLine("Error: unexpected C++ exception; FTP disabled for this attempt"); }
}
void InspectClientSeh(HMODULE client) noexcept {
    __try { InspectClientCaught(client); }
    __except(EXCEPTION_EXECUTE_HANDLER) { hlaftp::LogLine("Error: exception inspecting vrclient; FTP disabled for this attempt"); }
}
void MaybePatch() noexcept {
    if (!IsAlyx()) return;
    AcquireSRWLockExclusive(&patchLock);
    HMODULE client = nullptr;
    if (GetModuleHandleExW(0, L"vrclient_x64.dll", &client)) {
        InspectClientSeh(client);
        // /EHsc does not unwind C++ destructors across an SEH read fault.
        // Retain/release outside the fault-catching frame instead.
        FreeLibrary(client);
    }
    ReleaseSRWLockExclusive(&patchLock);
}
void Missing(int* error) noexcept { if (error) *error = 105; }
}

extern "C" unsigned int VR_InitInternal2(int* error, int type, const char* startup) {
    const auto function = Resolve<unsigned(*)(int*,int,const char*)>("VR_InitInternal2");
    if (!function) { Missing(error); return 0; }
    const auto token = function(error, type, startup);
    MaybePatch();
    return token;
}
extern "C" unsigned int VR_InitInternal(int* error, int type) {
    const auto function = Resolve<unsigned(*)(int*,int)>("VR_InitInternal");
    if (!function) { Missing(error); return 0; }
    const auto token = function(error, type);
    MaybePatch();
    return token;
}
extern "C" void* VR_GetGenericInterface(const char* version, int* error) {
    const auto function = Resolve<void*(*)(const char*,int*)>("VR_GetGenericInterface");
    if (!function) { Missing(error); return nullptr; }
    MaybePatch();
    auto* result = function(version, error);
    MaybePatch();
    return result;
}
extern "C" void VR_ShutdownInternal() {
    const auto function = Resolve<void(*)()>("VR_ShutdownInternal");
    if (IsAlyx()) {
        AcquireSRWLockExclusive(&patchLock);
        inspectedModule = nullptr;
        previousSite.reset();
        ReleaseSRWLockExclusive(&patchLock);
    }
    if (function) function();
}
// No loader callbacks, file I/O, or patching under the loader lock.
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
