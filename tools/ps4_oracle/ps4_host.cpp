// Native host for the PS4 tiling oracle: maps a decrypted PS4 eboot (ELF,
// x86-64 SysV) into this process and lets Python call into it through a
// SysV thunk. Only memcpy / memset / __stack_chk_fail / __stack_chk_guard are
// provided; the GpuAddress routines used by the oracle need nothing else.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" uint64_t sysv_call(void* fn, const uint64_t* args, uint64_t nargs);
extern "C" void sysv_memcpy();
extern "C" void sysv_memset();
extern "C" void sysv_trap();

static uint8_t* g_base = nullptr;
static uint64_t g_guard = 0x2B992DDFA23249D6ull;

static void patch_jump(uint64_t va, void* target)
{
    uint8_t* p = g_base + va;
    p[0] = 0x48; p[1] = 0xB8;                       // mov rax, imm64
    memcpy(p + 2, &target, 8);
    p[10] = 0xFF; p[11] = 0xE0;                     // jmp rax
}

extern "C" __declspec(dllexport) uint64_t ps4o_base() { return (uint64_t)g_base; }

// stubs: memcpy, memset, __stack_chk_fail import stub VAs; guard_got: GOT slot
// holding &__stack_chk_guard.
extern "C" __declspec(dllexport) int ps4o_init(const wchar_t* path, uint64_t stub_memcpy,
                                               uint64_t stub_memset, uint64_t stub_chk_fail,
                                               uint64_t guard_got)
{
    if (g_base) return 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return -1;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);

    uint64_t phoff = *(uint64_t*)&d[0x20];
    uint16_t phentsz = *(uint16_t*)&d[0x36], phnum = *(uint16_t*)&d[0x38];
    uint64_t top = 0;
    for (int i = 0; i < phnum; ++i) {
        const uint8_t* ph = &d[phoff + (size_t)i * phentsz];
        uint32_t type = *(uint32_t*)ph;
        uint64_t va = *(uint64_t*)(ph + 16), ms = *(uint64_t*)(ph + 40);
        if ((type == 1 || type == 0x61000010) && va + ms > top) top = va + ms;
    }
    g_base = (uint8_t*)VirtualAlloc(nullptr, (size_t)top + 0x1000, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (!g_base) return -2;
    for (int i = 0; i < phnum; ++i) {
        const uint8_t* ph = &d[phoff + (size_t)i * phentsz];
        uint32_t type = *(uint32_t*)ph;
        uint64_t off = *(uint64_t*)(ph + 8), va = *(uint64_t*)(ph + 16), fs = *(uint64_t*)(ph + 32);
        if (type == 1 || type == 0x61000010) memcpy(g_base + va, &d[off], (size_t)fs);
    }
    patch_jump(stub_memcpy, (void*)&sysv_memcpy);
    patch_jump(stub_memset, (void*)&sysv_memset);
    patch_jump(stub_chk_fail, (void*)&sysv_trap);
    uint64_t guard_ptr = (uint64_t)&g_guard;
    memcpy(g_base + guard_got, &guard_ptr, 8);
    FlushInstructionCache(GetCurrentProcess(), g_base, (size_t)top);
    return 0;
}

// Returns 0 on success (rax in *ret); nonzero = SEH exception code, with the
// faulting eboot-relative address in *fault_va.
extern "C" __declspec(dllexport) uint32_t ps4o_call(uint64_t va, const uint64_t* args, uint32_t nargs,
                                                    uint64_t* ret, uint64_t* fault_va)
{
    uint32_t code = 0;
    void* fault = nullptr;
    __try {
        *ret = sysv_call(g_base + va, args, nargs);
    } __except (code = GetExceptionCode(),
                fault = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                EXCEPTION_EXECUTE_HANDLER) {
        *fault_va = (uint64_t)((uint8_t*)fault - g_base);
        return code;
    }
    return 0;
}
