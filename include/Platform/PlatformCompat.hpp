#pragma once

//
// Platform compatibility layer for UE4SS on Linux.
// Provides Windows-like types and macros that map to Linux equivalents.
//

#ifdef __linux__

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <sys/mman.h>

// Windows type aliases for Linux
using DWORD = unsigned long;
using BOOL = int;
using LONG = long;
using HANDLE = void*;
using HMODULE = void*;
using LPVOID = void*;
using ULONG_PTR = unsigned long;
using FARPROC = void (*)();
using HRESULT = long;

// Windows constants
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(~0))
#endif
#ifndef NULL
#define NULL nullptr
#endif

// DLL loading equivalents
inline HMODULE LoadLibraryExW(const wchar_t* /*path*/, void* /*reserved*/, unsigned long /*flags*/) -> HMODULE {
    return nullptr; // Use dlopen directly in CppMod
}

inline FARPROC GetProcAddress(HMODULE module, const char* name) -> FARPROC {
    return reinterpret_cast<FARPROC>(dlsym(module, name));
}

inline void FreeLibrary(HMODULE module) -> void {
    dlclose(module);
}

// Error handling
inline DWORD GetLastError() -> DWORD {
    return static_cast<DWORD>(errno);
}

// Thread helpers
inline DWORD GetCurrentThreadId() -> DWORD {
    return static_cast<DWORD>(gettid());
}

inline DWORD GetCurrentProcessId() -> DWORD {
    return static_cast<DWORD>(getpid());
}

// String helpers that Windows provides
inline int printf_s(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}

inline int MessageBoxW(void* /*hWnd*/, const wchar_t* /*text*/, const wchar_t* /*caption*/, unsigned int /*type*/) -> int {
    return 0; // No-op on headless Linux servers
}

// SEH equivalents - no-op on Linux
#define __try
#define __except(x)
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0

#endif // __linux__
