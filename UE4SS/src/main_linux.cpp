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
#include <signal.h>

#include "UE4SSProgram.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <String/StringType.hpp>

using namespace RC;

static std::atomic<bool> s_ue4ss_initialized{false};
static UE4SSProgram* s_program = nullptr;

static auto get_module_path() -> std::filesystem::path
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&get_module_path), &info) && info.dli_fname)
    {
        return std::filesystem::path(info.dli_fname);
    }
    return std::filesystem::current_path() / "libUE4SS.so";
}

// Wait for the game's main executable to be fully loaded before initializing UE4SS.
// We do this by checking if /proc/self/exe is valid and the game binary is mapped.
static auto wait_for_game_ready() -> void
{
    // Give the game time to load its own libraries and initialize
    // This is critical - running too early will cause segfaults
    // because the game's memory layout isn't set up yet.
    sleep(2);

    // Verify we can read /proc/self/exe (game executable is loaded)
    char exe_path_buffer[1024]{};
    ssize_t len = readlink("/proc/self/exe", exe_path_buffer, sizeof(exe_path_buffer) - 1);
    if (len <= 0)
    {
        fprintf(stderr, "[UE4SS] Error: Cannot determine game executable path. Aborting.\n");
        return;
    }

    fprintf(stderr, "[UE4SS] Detected game executable: %s\n", exe_path_buffer);
}

static auto thread_dll_start() -> void
{
    try
    {
        wait_for_game_ready();

        auto module_path = get_module_path();
        fprintf(stderr, "[UE4SS] Library path: %s\n", module_path.string().c_str());

        s_program = new UE4SSProgram(module_path, {});
        s_program->init();

        if (auto e = s_program->get_error_object(); e->has_error())
        {
            if (!Output::has_internal_error())
            {
                Output::send<LogLevel::Error>(STR("Fatal Error: {}\n"), ensure_str(e->get_message()));
            }
            else
            {
                fprintf(stderr, "[UE4SS] Error: %s\n", e->get_message());
            }
        }

        s_ue4ss_initialized.store(true, std::memory_order_release);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[UE4SS] Exception during init: %s\n", e.what());
    }
    catch (...)
    {
        fprintf(stderr, "[UE4SS] Unknown exception during init\n");
    }
}

// This constructor runs when the shared library is loaded via LD_PRELOAD.
// We start UE4SS in a background thread with a delay to avoid crashing
// the game before its memory layout is fully initialized.
__attribute__((constructor))
static void ue4ss_linux_init()
{
    fprintf(stderr, "[UE4SS] Library loaded via LD_PRELOAD, starting initialization thread...\n");
    std::thread{thread_dll_start}.detach();
}

// Destructor runs when the shared library is unloaded
__attribute__((destructor))
static void ue4ss_linux_cleanup()
{
    fprintf(stderr, "[UE4SS] Cleaning up...\n");
    UE4SSProgram::static_cleanup();
    if (s_program)
    {
        delete s_program;
        s_program = nullptr;
    }
}

#endif // __linux__
