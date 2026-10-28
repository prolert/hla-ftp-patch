#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>

// Never executed: a small executable PE section for the real scanner to inspect.
#pragma section(".ftpcode", read, execute)
__declspec(allocate(".ftpcode")) const unsigned char code[] = {
    0x0f,0x2f,0xc8,                         // comiss xmm1,xmm0
    0x0f,0x86,0x37,0,0,0,                  // jbe code+0x40
    0x48,0x8d,0x0d,0,0,0,0,               // lea rcx,[rip+diagnostic]
    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0xc3
};
#pragma section(".ftpstr", read)
__declspec(allocate(".ftpstr")) const char diagnostic[] = "Activation threshold of %f cannot be less than the deactivation threshold of %f in filter %s.";
const char* volatile diagnosticAddress = diagnostic;

extern "C" __declspec(dllexport) void PrepareFixture() {
    DWORD protection = 0;
    auto* writable = const_cast<unsigned char*>(code);
    VirtualProtect(writable, sizeof(code), PAGE_EXECUTE_READWRITE, &protection);
    const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(diagnosticAddress) - reinterpret_cast<std::intptr_t>(code + 16));
    std::memcpy(writable + 12, &displacement, 4);
    DWORD ignored = 0;
    VirtualProtect(writable, sizeof(code), protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), writable, sizeof(code));
    if (GetEnvironmentVariableW(L"HLAFTP_FIXTURE_READ_FAULT", nullptr, 0)) {
        VirtualProtect(const_cast<char*>(diagnosticAddress), 1, PAGE_NOACCESS, &ignored);
    }
}
extern "C" __declspec(dllexport) unsigned int FixtureBranchByte() { return *reinterpret_cast<const volatile unsigned char*>(code + 3); }
