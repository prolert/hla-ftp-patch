#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace {
HMODULE client = nullptr;
std::wstring Directory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&Directory), &self);
    wchar_t path[32768]{};
    GetModuleFileNameW(self, path, 32768);
    std::wstring result(path);
    return result.substr(0, result.find_last_of(L"\\/") + 1);
}
}
extern "C" __declspec(dllexport) unsigned int VR_InitInternal2(int* error, int, const char*) {
    if (GetEnvironmentVariableW(L"HLAFTP_FIXTURE_FAIL_ONCE", nullptr, 0)) {
        SetEnvironmentVariableW(L"HLAFTP_FIXTURE_FAIL_ONCE", nullptr);
        if (error) *error = 108;
        return 0;
    }
    if (!client) {
        wchar_t realPath[32768]{};
        const auto realLength = GetEnvironmentVariableW(L"HLAFTP_REAL_VRCLIENT", realPath, 32768);
        client = LoadLibraryExW(realLength && realLength < 32768 ? realPath : (Directory() + L"vrclient_x64.dll").c_str(),
            nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!client) { if (error) *error = 105; return 0; }
        if (!realLength) reinterpret_cast<void(*)()>(GetProcAddress(client, "PrepareFixture"))();
    }
    if (error) *error = 0;
    return 123;
}
extern "C" __declspec(dllexport) unsigned int VR_InitInternal(int* error, int type) {
    return VR_InitInternal2(error, type, nullptr);
}
extern "C" __declspec(dllexport) void* VR_GetGenericInterface(const char*, int* error) {
    if (error) *error = client ? 0 : 109;
    return client ? reinterpret_cast<void*>(0x1234) : nullptr;
}
extern "C" __declspec(dllexport) void VR_ShutdownInternal() {
    if (client) { FreeLibrary(client); client = nullptr; }
}
extern "C" __declspec(dllexport) unsigned int VR_GetInitToken() { return 77; }
extern "C" __declspec(dllexport) const char* FixtureString() { return "fixture"; }
extern "C" __declspec(dllexport) void Dummy() {}
