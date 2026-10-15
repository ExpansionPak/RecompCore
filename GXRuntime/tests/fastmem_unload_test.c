// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <stdio.h>

static LONG CALLBACK probe(EXCEPTION_POINTERS* exception) {
    return exception->ExceptionRecord->ExceptionCode == 0xE0424242 ?
        EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2)
        return 1;
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll)
        return 2;
    int (*start)(void*) = (int (*)(void*))GetProcAddress(dll, "start_fastmem");
    if (!start)
        return 3;
    void* view = VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!view || !start(view))
        return 4;
    if (!FreeLibrary(dll))
        return 5;
    PVOID handler = AddVectoredExceptionHandler(0, probe);
    if (!handler)
        return 6;
    /* A stale fastmem VEH would jump into the unloaded DLL before probe. */
    RaiseException(0xE0424242, 0, 0, NULL);
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(view, 0, MEM_RELEASE);
    puts("module unload: exception chain intact");
    return 0;
}
