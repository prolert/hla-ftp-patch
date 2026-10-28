#include "patcher.h"
#include "guarded_write.h"
#include "pe_image.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace hlaftp;
namespace {
using Bytes = std::vector<uint8_t>;
constexpr size_t SectionTable = 0x188;
constexpr char Message[] = "Activation threshold of %f cannot be less than the deactivation threshold of %f in filter %s.";
void Require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
void U16(Bytes& b, size_t p, uint16_t value) { b.at(p) = static_cast<uint8_t>(value); b.at(p+1) = static_cast<uint8_t>(value >> 8); }
void U32(Bytes& b, size_t p, uint32_t value) { for (size_t i=0;i<4;++i) b.at(p+i)=static_cast<uint8_t>(value >> (8*i)); }
uint32_t Read32(const Bytes& b, size_t p) { uint32_t v=0; for(size_t i=0;i<4;++i) v |= static_cast<uint32_t>(b.at(p+i)) << (8*i); return v; }
void Lea(Bytes& b, size_t p, uint32_t target=0x2001) { b.at(p)=0x48; b.at(p+1)=0x8d; b.at(p+2)=0x0d; U32(b,p+3,target-static_cast<uint32_t>(p+7)); }
void Branch(Bytes& b, size_t p, uint32_t target, bool shortForm=false, bool rex=false) {
    b.at(p-3)=0x0f; b.at(p-2)=0x2f; b.at(p-1)=0xc8;
    if(rex) b.at(p-4)=0x44;
    if(shortForm) { b.at(p)=0x76; b.at(p+1)=static_cast<uint8_t>(target-static_cast<uint32_t>(p+2)); }
    else { b.at(p)=0x0f; b.at(p+1)=0x86; U32(b,p+2,target-static_cast<uint32_t>(p+6)); }
}
Bytes Fixture(bool shortForm=false) {
    Bytes b(0x3000,0); U16(b,0,0x5a4d); U32(b,0x3c,0x80); U32(b,0x80,0x4550);
    U16(b,0x84,0x8664); U16(b,0x86,2); U32(b,0x88,0x12345678); U16(b,0x94,0xf0);
    U16(b,0x98,0x20b); U32(b,0x98+32,0x1000); U32(b,0x98+36,0x200);
    U32(b,0x98+56,0x3000); U32(b,0x98+60,0x400);
    U32(b,SectionTable+8,0x400); U32(b,SectionTable+12,0x1000); U32(b,SectionTable+16,0x400); U32(b,SectionTable+20,0x400); U32(b,SectionTable+36,0x60000020);
    U32(b,SectionTable+40+8,0x400); U32(b,SectionTable+40+12,0x2000); U32(b,SectionTable+40+16,0x400); U32(b,SectionTable+40+20,0x800); U32(b,SectionTable+40+36,0x40000040);
    std::fill(b.begin()+0x1000,b.begin()+0x1400,uint8_t{0x90});
    std::memcpy(b.data()+0x2001,Message,sizeof(Message));
    Lea(b,0x1080); Branch(b,0x1050,shortForm ? 0x10a0:0x1100,shortForm);
    return b;
}
Bytes Raw(const Bytes& mapped) {
    Bytes raw(0xc00); std::copy_n(mapped.begin(),0x400,raw.begin());
    std::copy_n(mapped.begin()+0x1000,0x400,raw.begin()+0x400);
    std::copy_n(mapped.begin()+0x2000,0x400,raw.begin()+0x800); return raw;
}
FindResult Find(const Bytes& b) { return FindPatchSite(b.data(),b.size()); }
void Status(const Bytes& b, PatchStatus expected) { const auto r=Find(b); if(r.status!=expected) throw std::runtime_error(std::string("expected ")+StatusName(expected)+", got "+StatusName(r.status)+": "+r.detail); }
void Throws(const std::function<void()>& f) { bool caught=false; try { f(); } catch(const std::runtime_error&) { caught=true; } Require(caught,"expected runtime_error"); }
std::vector<std::pair<std::string,std::function<void()>>> tests;
void Test(const char* name, std::function<void()> f) { tests.emplace_back(name,std::move(f)); }
void RegisterTests() {
    Test("long branch preserves target and finder is read only",[] { auto b=Fixture(); auto original=b; auto r=Find(b); Require(r.status==PatchStatus::Found,"not found"); Require(b==original,"finder modified image"); Require(r.site->rva==0x1050,"wrong RVA"); Require(r.site->before==Bytes({0x0f,0x86,0xaa,0,0,0}),"before bytes"); Require(r.site->after==Bytes({0xe9,0xab,0,0,0,0x90}),"target changed"); });
    Test("short branch preserves target",[] { auto b=Fixture(true); auto r=Find(b); Require(r.status==PatchStatus::Found,"short not found"); Require(r.site->after==Bytes({0xeb,0x4e}),"short target changed"); });
    Test("optional REX on comiss",[] { auto b=Fixture(); Branch(b,0x1050,0x1100,false,true); Status(b,PatchStatus::Found); });
    Test("REX lea high register",[] { auto b=Fixture(); b[0x1080]=0x4c; Status(b,PatchStatus::Found); });
    Test("already patched long",[] { auto b=Fixture(); auto r=Find(b); std::copy(r.site->after.begin(),r.site->after.end(),b.begin()+0x1050); Status(b,PatchStatus::AlreadyPatched); });
    Test("already patched short",[] { auto b=Fixture(true); b[0x1050]=0xeb; Status(b,PatchStatus::AlreadyPatched); });
    Test("long jmp must have nop padding",[] { auto b=Fixture(); b[0x1050]=0xe9; U32(b,0x1051,0xab); b[0x1055]=0xcc; Status(b,PatchStatus::PatternChanged); });
    Test("missing anchor",[] { auto b=Fixture(); std::fill(b.begin()+0x2000,b.begin()+0x2400,uint8_t{0}); Status(b,PatchStatus::NotNeeded); });
    Test("anchor in executable section ignored",[] { auto b=Fixture(); U32(b,SectionTable+40+36,0x60000020); Status(b,PatchStatus::NotNeeded); });
    Test("missing xref",[] { auto b=Fixture(); b[0x1081]=0x89; Status(b,PatchStatus::PatternChanged); });
    Test("xref must reference string start",[] { auto b=Fixture(); Lea(b,0x1080,0x201c); Status(b,PatchStatus::PatternChanged); });
    Test("duplicate string starts",[] { auto b=Fixture(); std::memcpy(b.data()+0x2101,Message,sizeof(Message)); Status(b,PatchStatus::PatternChanged); });
    Test("duplicate phrase within same C string is one start",[] { auto b=Fixture(); const std::string s=std::string(Message)+Message; std::memcpy(b.data()+0x2001,s.c_str(),s.size()+1); Status(b,PatchStatus::Found); });
    Test("duplicate xrefs",[] { auto b=Fixture(); Lea(b,0x1090); Status(b,PatchStatus::PatternChanged); });
    Test("data xref ignored",[] { auto b=Fixture(); Lea(b,0x2200); Status(b,PatchStatus::Found); });
    Test("duplicate branch candidates",[] { auto b=Fixture(); Branch(b,0x1060,0x1110); Status(b,PatchStatus::PatternChanged); });
    Test("mixed patched and unpatched candidates",[] { auto b=Fixture(true); Branch(b,0x1060,0x10b0,true); b[0x1060]=0xeb; Status(b,PatchStatus::PatternChanged); });
    Test("no comiss",[] { auto b=Fixture(); b[0x104e]=0x2e; Status(b,PatchStatus::PatternChanged); });
    Test("memory comiss rejected",[] { auto b=Fixture(); b[0x104f]=0x08; Status(b,PatchStatus::PatternChanged); });
    Test("COMISD prefix rejected",[] { auto b=Fixture(); b[0x104c]=0x66; Status(b,PatchStatus::PatternChanged); });
    Test("invalid repeated scalar prefix rejected",[] { auto b=Fixture(); b[0x104c]=0xf3; Status(b,PatchStatus::PatternChanged); });
    Test("COMISD prefix before REX rejected",[] { auto b=Fixture(); b[0x104b]=0x66; b[0x104c]=0x44; Status(b,PatchStatus::PatternChanged); });
    Test("different conditional rejected",[] { auto b=Fixture(); b[0x1051]=0x87; Status(b,PatchStatus::PatternChanged); });
    Test("branch target before lea rejected",[] { auto b=Fixture(); Branch(b,0x1050,0x1070); Status(b,PatchStatus::PatternChanged); });
    Test("branch target exactly lea rejected",[] { auto b=Fixture(); Branch(b,0x1050,0x1080); Status(b,PatchStatus::PatternChanged); });
    Test("branch target outside forward window rejected",[] { auto b=Fixture(); Branch(b,0x1050,0x1281); Status(b,PatchStatus::PatternChanged); });
    Test("branch target at forward limit accepted",[] { auto b=Fixture(); Branch(b,0x1050,0x1280); Status(b,PatchStatus::Found); });
    Test("branch outside backwards window rejected",[] { auto b=Fixture(); std::fill(b.begin()+0x104c,b.begin()+0x1056,uint8_t{0x90}); Branch(b,0x1000,0x1100); Status(b,PatchStatus::PatternChanged); });
    Test("branch target outside same executable section",[] { auto b=Fixture(); U32(b,SectionTable+8,0x90); U32(b,SectionTable+16,0x90); Status(b,PatchStatus::PatternChanged); });
    Test("string without terminating nul rejected",[] { auto b=Fixture(); std::fill(b.begin()+0x2001+sizeof(Message)-1,b.begin()+0x2400,uint8_t{'x'}); Status(b,PatchStatus::PatternChanged); });
    Test("string starts at section boundary accepted",[] { auto b=Fixture(); std::fill(b.begin()+0x2000,b.begin()+0x2400,uint8_t{0}); std::memcpy(b.data()+0x2000,Message,sizeof(Message)); Lea(b,0x1080,0x2000); Status(b,PatchStatus::Found); });
    Test("string must not borrow nul from next section",[] { auto b=Fixture(); std::fill(b.begin()+0x2000,b.begin()+0x2400,uint8_t{'x'}); std::memcpy(b.data()+0x2400-sizeof(Message)+1,Message,sizeof(Message)-1); Status(b,PatchStatus::PatternChanged); });
    Test("empty and null image",[] { Status({},PatchStatus::Error); Require(FindPatchSite(nullptr,0x3000).status==PatchStatus::Error,"null"); });
    Test("parse reports timestamp and sections",[] { auto p=ParseMappedImage(Fixture()); Require(p.timestamp==0x12345678 && p.sizeOfImage==0x3000,"metadata"); Require(p.sections.size()==2 && p.sections[0].executable && !p.sections[1].executable,"sections"); });
    Test("raw mapping copies sections and zeroes virtual tail",[] { auto b=Fixture(); U32(b,SectionTable+8,0x500); auto m=MapFileImage(Raw(b)); Require(m.size()==0x3000 && m[0x1400]==0 && Find(m).status==PatchStatus::Found,"map mismatch"); });
    const std::vector<std::pair<const char*,std::function<void(Bytes&)>>> corruptions={
      {"bad DOS magic",[](Bytes&b){b[0]=0;}}, {"bad PE signature",[](Bytes&b){b[0x80]=0;}},
      {"invalid e_lfanew",[](Bytes&b){U32(b,0x3c,0xfffffff0);}}, {"PE32 rejected",[](Bytes&b){U16(b,0x98,0x10b);}},
      {"non AMD64 machine",[](Bytes&b){U16(b,0x84,0x14c);}}, {"zero sections",[](Bytes&b){U16(b,0x86,0);}},
      {"too many sections",[](Bytes&b){U16(b,0x86,0xffff);}}, {"truncated optional header",[](Bytes&b){U16(b,0x94,60);}},
      {"image smaller than section",[](Bytes&b){U32(b,0x98+56,0x2100);}}, {"headers smaller than section table",[](Bytes&b){U32(b,0x98+60,0x180);}},
      {"headers exceed image",[](Bytes&b){U32(b,0x98+60,0x4000);}}, {"overflowing section",[](Bytes&b){U32(b,SectionTable+12,0xffffff00);}},
      {"overlapping sections",[](Bytes&b){U32(b,SectionTable+40+12,0x1200);}}, {"section overlaps headers",[](Bytes&b){U32(b,SectionTable+12,0x200);}},
      {"declared image exceeds mapped buffer",[](Bytes&b){U32(b,0x98+56,0x4000);}}
    };
    for(const auto& [name,change]:corruptions) tests.emplace_back(name,[change] { auto b=Fixture(); change(b); Status(b,PatchStatus::Error); Throws([&] { (void)ParseMappedImage(b); }); });
    Test("truncated header inputs",[] { auto b=Fixture(); for(size_t size: {size_t{1},size_t{63},size_t{0x80},size_t{0x188},size_t{0x1d7},size_t{0x2fff}}) Require(FindPatchSite(b.data(),size).status==PatchStatus::Error,"truncation accepted"); });
    Test("truncated raw data rejected",[] { auto b=Raw(Fixture()); b.resize(0xbff); Throws([&]{(void)MapFileImage(b);}); });
    Test("raw data overlap rejected",[] { auto b=Raw(Fixture()); U32(b,SectionTable+40+20,0x600); Throws([&]{(void)MapFileImage(b);}); });
    Test("raw data overlaps headers rejected",[] { auto b=Raw(Fixture()); U32(b,SectionTable+20,0x200); Throws([&]{(void)MapFileImage(b);}); });
    Test("raw data offset overflows rejected",[] { auto b=Raw(Fixture()); U32(b,SectionTable+20,0xfffffff0); Throws([&]{(void)MapFileImage(b);}); });
    Test("status names stable",[] { Require(std::string(StatusName(PatchStatus::Patched))=="Patched","name"); Require(std::string(StatusName(PatchStatus::Error))=="Error","name"); });
#ifdef _WIN32
    Test("guarded write catches inaccessible read and write",[] {
        auto* page=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        Require(page!=nullptr,"allocate guarded write test"); page[0]=0x76; page[1]=0x20;
        const uint8_t before[]{0x76,0x20},after[]{0xeb,0x20}; DWORD old=0;
        VirtualProtect(page,0x1000,PAGE_READONLY,&old);
        const auto writeFault=detail::GuardedWrite(page,before,after,2);
        const bool unchanged=page[0]==0x76 && page[1]==0x20;
        VirtualProtect(page,0x1000,PAGE_NOACCESS,&old);
        const auto readFault=detail::GuardedWrite(page,before,after,2);
        VirtualFree(page,0,MEM_RELEASE);
        Require(writeFault==detail::WriteResult::Fault && readFault==detail::WriteResult::Fault,"SEH must return control for page cleanup");
        Require(unchanged,"fault changed read-only memory");
    });
    Test("apply restores distinct protection across page boundary",[] {
        auto b=Fixture(); b.resize(0x5000); U32(b,0x98+56,0x5000); U32(b,SectionTable+8,0x2000);
        U32(b,SectionTable+40+12,0x4000); std::fill(b.begin()+0x1000,b.begin()+0x3000,uint8_t{0x90});
        std::memcpy(b.data()+0x4001,Message,sizeof(Message)); Lea(b,0x2020,0x4001); Branch(b,0x1ffd,0x20a0);
        auto* p=static_cast<uint8_t*>(VirtualAlloc(nullptr,b.size(),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)); Require(p!=nullptr,"VirtualAlloc"); std::memcpy(p,b.data(),b.size());
        DWORD old=0; VirtualProtect(p+0x1000,0x1000,PAGE_EXECUTE_READ,&old); VirtualProtect(p+0x2000,0x1000,PAGE_READONLY,&old);
        const auto r=ApplyPatch(p,b.size()); MEMORY_BASIC_INFORMATION first{},second{}; VirtualQuery(p+0x1ffd,&first,sizeof(first)); VirtualQuery(p+0x2000,&second,sizeof(second));
        const bool changed=p[0x1ffd]==0xe9 && p[0x2002]==0x90; VirtualFree(p,0,MEM_RELEASE);
        Require(r.status==PatchStatus::Patched && changed,"cross page patch"); Require(first.Protect==PAGE_EXECUTE_READ && second.Protect==PAGE_READONLY,"cross page protection changed");
    });
    for(bool shortForm: {false,true}) tests.emplace_back(shortForm ? "apply short executable image" : "apply long executable image",[shortForm] {
        auto b=Fixture(shortForm); auto* p=static_cast<uint8_t*>(VirtualAlloc(nullptr,b.size(),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)); Require(p!=nullptr,"VirtualAlloc");
        std::memcpy(p,b.data(),b.size()); DWORD old=0; Require(VirtualProtect(p,b.size(),PAGE_EXECUTE_READ,&old)!=0,"protect fixture");
        auto r=ApplyPatch(p,b.size()); const auto second=ApplyPatch(p,b.size()); MEMORY_BASIC_INFORMATION info{}; VirtualQuery(p+0x1050,&info,sizeof(info));
        const bool bytesCorrect=p[0x1050]==(shortForm?0xeb:0xe9); const bool protectRestored=info.Protect==PAGE_EXECUTE_READ; VirtualFree(p,0,MEM_RELEASE);
        Require(r.status==PatchStatus::Patched,"apply did not patch"); Require(second.status==PatchStatus::AlreadyPatched,"second apply"); Require(bytesCorrect,"wrong write"); Require(protectRestored,"protection not restored");
    });
#endif
    Test("apply error leaves malformed bytes unchanged",[] { Bytes b(8,0); const auto original=b; Require(ApplyPatch(b.data(),b.size()).status==PatchStatus::Error,"apply malformed"); Require(b==original,"wrote malformed"); });
}
void RealFile(const char* variable, PatchStatus expected) {
    std::string path;
#ifdef _WIN32
    const DWORD required=GetEnvironmentVariableA(variable,nullptr,0);
    if(required) { path.resize(required); GetEnvironmentVariableA(variable,path.data(),required); path.resize(required-1); }
#else
    if(const char* value=std::getenv(variable)) path=value;
#endif
    if(path.empty()) { std::cout<<"SKIP "<<variable<<" (not set)\n"; return; }
    std::ifstream input(std::filesystem::path(path),std::ios::binary); Require(input.good(),"cannot open real fixture"); Bytes raw((std::istreambuf_iterator<char>(input)),{});
    auto mapped=MapFileImage(raw); auto r=Find(mapped); Require(r.status==expected,r.detail.c_str());
    if(expected==PatchStatus::Found && r.site && r.site->rva==0x3269cb) {
        Require(r.site->before==Bytes({0x0f,0x86,0x8b,0,0,0}),"known current original mismatch"); Require(r.site->after==Bytes({0xe9,0x8c,0,0,0,0x90}),"known current patch mismatch");
    }
    std::cout<<"REAL "<<variable<<" "<<StatusName(r.status); if(r.site) std::cout<<" RVA 0x"<<std::hex<<r.site->rva<<std::dec; std::cout<<'\n';
}
}
int main() {
    RegisterTests(); unsigned failed=0;
    for(const auto& [name,f]:tests) { try { f(); std::cout<<"PASS "<<name<<'\n'; } catch(const std::exception&e) { ++failed; std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n'; } }
    try { RealFile("HLAFTP_CURRENT_VRCLIENT",PatchStatus::Found); RealFile("HLAFTP_OLD_VRCLIENT",PatchStatus::NotNeeded); } catch(const std::exception&e) { ++failed; std::cerr<<"FAIL real fixture: "<<e.what()<<'\n'; }
    std::cout<<tests.size()<<" synthetic tests, "<<failed<<" failures\n"; return failed?1:0;
}
