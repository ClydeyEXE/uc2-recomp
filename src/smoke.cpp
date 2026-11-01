// Confirms the toolchain works and probes how the Xbox address space could be hosted.
#include <windows.h>
#include <cstdio>

int main() {
    printf("pointer size: %zu bits\n", sizeof(void*) * 8);
#ifdef _WIN64
    // A lifted x64 build addresses guest memory as host_base + guest_va.
    void* p = VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    printf("reserve flat 4GB guest space: %s (%p)\n", p ? "ok" : "FAILED", p);
    return p ? 0 : 1;
#else
    // A 32-bit build could keep the game's original addresses.
    void* p = VirtualAlloc((void*)0x10000, 0x46A000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    printf("map XBE range at 0x10000: %s (%p, error %lu)\n", p ? "ok" : "FAILED", p, p ? 0 : GetLastError());
    return p ? 0 : 1;
#endif
}
