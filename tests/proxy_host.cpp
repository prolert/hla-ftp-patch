#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Resolve(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    Require(address != nullptr, name);
    return reinterpret_cast<T>(address);
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected alyx/other argument");
        const bool real = std::string(argv[1]) == "real";
        const bool fault = std::string(argv[1]) == "fault";
        const bool alyx = real || fault || std::string(argv[1]) == "alyx";
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        const auto directory = std::filesystem::path(path).parent_path();
        const auto log = directory / "hla_ftp.log";
        std::filesystem::remove(log);
        auto module = LoadLibraryW((directory / "openvr_api.dll").c_str());
        Require(module != nullptr, "proxy must load");
        const char* exports[] = {"LiquidVR","VRCompositorSystemInternal","VRControlPanel","VRHeadsetView",
            "VROculusDirect","VRPaths","VRRenderModelsInternal","VRSceneGraph","VRTrackedCameraInternal",
            "VRVirtualDisplay","VR_GetGenericInterface","VR_GetInitToken","VR_GetRuntimePath","VR_GetStringForHmdError",
            "VR_GetVRInitErrorAsEnglishDescription","VR_GetVRInitErrorAsSymbol","VR_InitInternal","VR_InitInternal2",
            "VR_IsHmdPresent","VR_IsInterfaceVersionValid","VR_IsRuntimeInstalled","VR_RuntimePath","VR_ShutdownInternal"};
        for (unsigned i = 0; i < 23; ++i) {
            Require(GetProcAddress(module, exports[i]) != nullptr, exports[i]);
            Require(GetProcAddress(module, MAKEINTRESOURCEA(i + 1)) == GetProcAddress(module, exports[i]), "ordinal mismatch");
        }
        auto init = Resolve<unsigned(*)(int*,int,const char*)>(module,"VR_InitInternal2");
        auto legacy = Resolve<unsigned(*)(int*,int)>(module,"VR_InitInternal");
        auto generic = Resolve<void*(*)(const char*,int*)>(module,"VR_GetGenericInterface");
        auto shutdown = Resolve<void(*)()>(module,"VR_ShutdownInternal");
        int error = -1;
        Require(generic("IVRInput_011", &error) == nullptr, "pre-init forwarding");
        SetEnvironmentVariableW(L"HLAFTP_FIXTURE_FAIL_ONCE",L"1");
        Require(init(&error,1,nullptr) == 0 && error == 108, "preserve original init failure");
        if (fault) SetEnvironmentVariableW(L"HLAFTP_FIXTURE_READ_FAULT", L"1");
        Require(init(&error,1,"fixture") == 123 && error == 0, "preserve original init result");
        auto client = GetModuleHandleW(L"vrclient_x64.dll");
        Require(client != nullptr, "fixture client loaded");
        if (fault) {
            Require(Resolve<unsigned(*)()>(client,"FixtureBranchByte")() == 0x0f, "fault must not patch");
            Require(generic("IVRInput_011", &error) == reinterpret_cast<void*>(0x1234), "inspection fault preserves original interface");
            shutdown();
            Require(GetModuleHandleW(L"vrclient_x64.dll") == nullptr, "SEH inspection fault must release vrclient module references");
            std::cout << "PASS: SEH read fault preserves forwarding and releases module references\n";
            return 0;
        }
        if (real) {
            const auto byte = *reinterpret_cast<const volatile unsigned char*>(reinterpret_cast<const unsigned char*>(client) + 0x3269cb);
            Require(byte == 0xe9, "real loaded SteamVR branch must be patched in memory");
            Require(generic("IVRInput_011", &error) == reinterpret_cast<void*>(0x1234), "real module loader fixture forwards interface");
            shutdown();
            std::ifstream input(log);
            std::string contents((std::istreambuf_iterator<char>(input)), {});
            Require(contents.find("Patched") != std::string::npos && contents.find("3269cb") != std::string::npos, "real loaded module logged");
            std::cout << "PASS: actual loaded SteamVR patched in isolated hlvr fixture; no VR initialization\n";
            return 0;
        }
        auto branch = Resolve<unsigned(*)()>(client,"FixtureBranchByte");
        Require(branch() == (alyx ? 0xe9u : 0x0fu), "patch only inside hlvr before returning init");
        Require(generic("IVRInput_011", &error) == reinterpret_cast<void*>(0x1234), "interface forwarded");
        Require(Resolve<unsigned(*)()>(module,"VR_GetInitToken")() == 77, "forwarded token");
        Require(std::string(Resolve<const char*(*)(int)>(module,"VR_GetVRInitErrorAsSymbol")(0)) == "fixture", "forwarded string");
        shutdown();
        Require(legacy(&error,1) == 123 && error == 0, "legacy initialization forwarded");
        client = GetModuleHandleW(L"vrclient_x64.dll");
        Require(Resolve<unsigned(*)()>(client,"FixtureBranchByte")() == (alyx ? 0xe9u : 0x0fu), "patch after shutdown/reload");
        shutdown();
        Require(std::filesystem::exists(log) == alyx, "only Alyx writes log");
        if (alyx) {
            std::ifstream input(log);
            std::string contents((std::istreambuf_iterator<char>(input)), {});
            Require(contents.find("Patched") != std::string::npos, "log patch status");
        }
        std::cout << "PASS: " << (alyx ? "Alyx" : "other process") << " proxy integration\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (Win32=" << GetLastError() << ")\n";
        return 1;
    }
}
