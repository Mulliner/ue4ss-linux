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
#include <setjmp.h>

#include "UE4SSProgram.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <String/StringType.hpp>

using namespace RC;

static std::atomic<bool> s_ue4ss_initialized{false};
static UE4SSProgram* s_program = nullptr;

// SIGSEGV recovery for UE4SS init thread
static thread_local sigjmp_buf s_init_jmpbuf;
static thread_local bool s_has_jmpbuf = false;
static struct sigaction s_old_sigsegv;
static struct sigaction s_old_sigbus;

static void ue4ss_sigsegv_handler(int sig, siginfo_t* info, void* ucontext)
{
    (void)info; (void)ucontext;
    if (s_has_jmpbuf)
    {
        fprintf(stderr, "[UE4SS] Caught signal %d during init, recovering...\n", sig);
        siglongjmp(s_init_jmpbuf, sig);
    }
    // No jump buffer - restore original handler and re-raise
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    raise(sig);
}

static auto install_signal_handlers() -> void
{
    struct sigaction sa{};
    sa.sa_sigaction = ue4ss_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_old_sigsegv);
    sigaction(SIGBUS, &sa, &s_old_sigbus);
}

static auto restore_signal_handlers() -> void
{
    sigaction(SIGSEGV, &s_old_sigsegv, nullptr);
    sigaction(SIGBUS, &s_old_sigbus, nullptr);
}

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
static auto wait_for_game_ready() -> void
{
    // Wait for the game to fully initialize its memory layout.
    // UE5 games (like Palworld) need significant time to load.
    // We wait in stages and check if the game is still alive.
    fprintf(stderr, "[UE4SS] Waiting for game to initialize...\n");
    for (int i = 0; i < 10; ++i)
    {
        sleep(1);
        fprintf(stderr, "[UE4SS] Waiting... (%d/10)\n", i + 1);
    }

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
    // Install our signal handlers so we can recover from segfaults during init
    install_signal_handlers();

    int sig = sigsetjmp(s_init_jmpbuf, 1);
    if (sig != 0)
    {
        fprintf(stderr, "[UE4SS] Recovered from signal %d. UE4SS init failed but game should continue.\n", sig);
        restore_signal_handlers();
        return;
    }
    s_has_jmpbuf = true;

    try
    {
        wait_for_game_ready();

        auto module_path = get_module_path();
        fprintf(stderr, "[UE4SS] Library path: %s\n", module_path.string().c_str());

        fprintf(stderr, "[UE4SS] Creating UE4SSProgram instance...\n");
        s_program = new UE4SSProgram(module_path, {});

        fprintf(stderr, "[UE4SS] Calling init()...\n");
        s_program->init();
        fprintf(stderr, "[UE4SS] init() completed successfully.\n");

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
        fprintf(stderr, "[UE4SS] Initialization complete.\n");
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[UE4SS] Exception during init: %s\n", e.what());
    }
    catch (...)
    {
        fprintf(stderr, "[UE4SS] Unknown exception during init\n");
    }

    s_has_jmpbuf = false;
    restore_signal_handlers();
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
    if (s_ue4ss_initialized.load(std::memory_order_acquire))
    {
        fprintf(stderr, "[UE4SS] Cleaning up...\n");
        UE4SSProgram::static_cleanup();
        if (s_program)
        {
            delete s_program;
            s_program = nullptr;
        }
    }
}

#endif // __linux__
