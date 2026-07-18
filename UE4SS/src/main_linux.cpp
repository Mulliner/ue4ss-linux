//
// Linux entry point for UE4SS.
// On Linux, UE4SS is loaded via LD_PRELOAD as a shared library.
// The constructor attribute ensures this code runs when the library is loaded.
//

#ifdef __linux__

#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <atomic>
#include <dlfcn.h>
#include <unistd.h>
#include <string>
#include <filesystem>

#include "UE4SSProgram.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <String/StringType.hpp>

using namespace RC;

static std::atomic<bool> s_ue4ss_initialized{false};

static auto thread_dll_start(UE4SSProgram* program) -> void
{
    program->init();

    if (auto e = program->get_error_object(); e->has_error())
    {
        if (!Output::has_internal_error())
        {
            Output::send<LogLevel::Error>(STR("Fatal Error: {}\n"), ensure_str(e->get_message()));
        }
        else
        {
            fprintf(stderr, "Error: %s\n", e->get_message());
        }
    }

    s_ue4ss_initialized.store(true, std::memory_order_release);
}

static auto get_module_path() -> std::filesystem::path
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&get_module_path), &info) && info.dli_fname)
    {
        return std::filesystem::path(info.dli_fname).parent_path();
    }
    return std::filesystem::current_path();
}

// This constructor runs when the shared library is loaded via LD_PRELOAD
__attribute__((constructor))
static void ue4ss_linux_init()
{
    // Get the directory of this shared library as the working directory
    auto module_path = get_module_path();

    // Convert path to CharType (char16_t on Linux due to FORCE_U16)
    auto path_str = module_path.wstring();
    auto program = new UE4SSProgram(path_str, {});

    // Start UE4SS in a new thread so we don't block the game's initialization
    std::thread{thread_dll_start, program}.detach();
}

// Destructor runs when the shared library is unloaded
__attribute__((destructor))
static void ue4ss_linux_cleanup()
{
    UE4SSProgram::static_cleanup();
}

#endif // __linux__
