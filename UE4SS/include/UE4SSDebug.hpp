#pragma once

#include <cstdio>

// Global debug logging control for UE4SS on Linux.
// Controlled by the UE4SS_DEBUG environment variable.
// When UE4SS_DEBUG is not set (or set to 0), only warnings and errors are printed.
// When UE4SS_DEBUG=1, all debug messages are printed.
// When UE4SS_DEBUG=2, even more verbose output is printed.

namespace UE4SSDebug
{
    inline int get_debug_level()
    {
        static int level = []() {
            const char* env = getenv("UE4SS_DEBUG");
            if (env) return atoi(env);
            return 0;
        }();
        return level;
    }
}

// UE4SS_LOG: Always printed (important messages, mod output)
#define UE4SS_LOG(...) fprintf(stderr, __VA_ARGS__)

// UE4SS_DBG: Only printed when UE4SS_DEBUG >= 1
#define UE4SS_DBG(...) do { if (UE4SSDebug::get_debug_level() >= 1) { fprintf(stderr, __VA_ARGS__); } } while(0)

// UE4SS_VDBG: Only printed when UE4SS_DEBUG >= 2 (very verbose)
#define UE4SS_VDBG(...) do { if (UE4SSDebug::get_debug_level() >= 2) { fprintf(stderr, __VA_ARGS__); } } while(0)

// UE4SS_ERR: Always printed (errors)
#define UE4SS_ERR(...) fprintf(stderr, __VA_ARGS__)
