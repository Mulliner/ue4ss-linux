// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Unreal/Core/CoreTypes.hpp"
#include "Unreal/Core/HAL/PlatformMemory.hpp"
#include "Unreal/Core/GenericPlatform/GenericPlatformMisc.hpp"

#define UE_DEBUG_BREAK_IMPL() PLATFORM_BREAK()

namespace RC::Unreal
{
    struct FLinuxPlatformMisc
        : public FGenericPlatformMisc
    {
        FORCEINLINE static void MemoryBarrier()
        {
            __sync_synchronize();
        }
    };

    typedef FLinuxPlatformMisc FPlatformMisc;
}
