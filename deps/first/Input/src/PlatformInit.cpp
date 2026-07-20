#include <Input/PlatformInputSource.hpp>
#ifdef _WIN32
#include <Input/Platform/Win32AsyncInputSource.hpp>
#endif
#ifdef HAS_GUI
#include <Input/Platform/GLFW3InputSource.hpp>
#endif

namespace RC::Input
{
    auto Handler::init() -> void
    {
#ifdef _WIN32
        register_input_source(std::make_shared<Win32AsyncInputSource>(L"ConsoleWindowClass", L"UnrealWindow"));
#endif
#ifdef HAS_GUI
        register_input_source(std::make_shared<GLFW3InputSource>());
#endif
    }
} // namespace RC::Input