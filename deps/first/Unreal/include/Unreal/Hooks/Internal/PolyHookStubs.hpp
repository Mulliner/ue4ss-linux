#pragma once

// Linux stubs for PolyHook2 types to allow compilation without PolyHook2
// These provide the same interface but do nothing - hooks won't work on Linux

#ifndef _WIN32

#include <cstdint>

namespace PLH
{
    class x64Detour
    {
    public:
        x64Detour(uint64_t fnAddress, uint64_t fnCallback, uint64_t* trampoline) {}
        bool hook() { return false; }
        bool unHook() { return false; }
        bool isHooked() const { return false; }
        uint64_t getOriginal() const { return 0; }
    };

    template<typename DestType, typename SrcType>
    inline DestType FnCast(SrcType src, DestType fallback)
    {
        return fallback;
    }
}

#endif
