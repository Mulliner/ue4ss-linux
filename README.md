# UE4SS Linux Native Port

A native Linux build of UE4SS (Unreal Engine 4/5 Scripting System) for dedicated game servers running on Linux. This port enables Lua mod loading and scripting on Linux dedicated servers without requiring Windows.

## Features

- **Lua Mod Loading**: Load and run Lua-based mods on Linux dedicated servers
- **LD_PRELOAD Injection**: Loaded via `LD_PRELOAD` — no proxy DLL needed
- **Per-Mod Crash Recovery**: If one mod crashes (SIGSEGV), other mods continue to load
- **Headless Mode**: GUI and input systems disabled for server environments
- **POSIX File System**: Full Linux file system support
- **Native Linux Crash Dumper**: Signal-based crash handling with backtrace

## Installation

### Prerequisites

- A Linux dedicated server for a UE4/5 game (e.g., Palworld, Ark, etc.)
- Root or sudo access to the server

### Steps

1. **Download the latest release**

   Download `UE4SS-Linux-build.zip` from the [Releases page](https://github.com/XarminaEu/ue4ss-linux/releases/latest).

2. **Extract the archive**

   ```bash
   unzip UE4SS-Linux-build.zip
   ```

   This will extract `libUE4SS.so`.

3. **Copy `libUE4SS.so` to your game's binary directory**

   Place `libUE4SS.so` in the same directory as your game server executable.

   Example for Palworld:
   ```bash
   cp libUE4SS.so /path/to/PalServer/Binaries/Linux/
   ```

4. **Set up the Mods directory**

   Create a `Mods` folder in the game's binary directory:
   ```bash
   mkdir -p /path/to/PalServer/Binaries/Linux/Mods
   ```

   Create a `mods.txt` file inside the `Mods` folder to specify which mods to load:
   ```bash
   echo "UE4SSStatus : 1" > /path/to/PalServer/Binaries/Linux/Mods/mods.txt
   ```

   Each line follows the format `ModName : 1` (enabled) or `ModName : 0` (disabled).

5. **Set up Lua mods**

   Each mod goes in its own folder under `Mods/`:
   ```
   Mods/
   ├── mods.txt
   └── MyMod/
       └── scripts/
           └── main.lua
   ```

   Note: On Linux, the scripts directory is lowercase `scripts` (case-sensitive).

6. **Launch the server with LD_PRELOAD**

   Set the `LD_PRELOAD` environment variable to load UE4SS:
   ```bash
   LD_PRELOAD=/path/to/libUE4SS.so ./PalServer-Linux-Shipping
   ```

   Or set it in your server startup script / systemd service:
   ```bash
   export LD_PRELOAD=/path/to/libUE4SS.so
   ```

### Optional: UE4SS Settings

Create a `UE4SS-settings.ini` file in the game's binary directory to configure UE4SS:

```ini
[General]
EnableHotReloadSystem=true
EnableAutoReloadingLuaMods=true
UseCache=true
InvalidateCacheIfDLLDiffers=true
EnableDebugKeyBindings=false
```

On Linux, if no settings file is found, hardcoded defaults are used.

## Verified Games

- **Palworld** (UE5.1) — Dedicated server on Linux

## Known Limitations

- **Limited Mode**: UE function addresses are not resolved on stripped Linux binaries. Mod functionality is limited to Lua scripting and basic operations.
- **No GUI**: GUI is disabled in the Linux build (headless mode only).
- **No Blueprint Mod Loader**: Blueprint mod loading requires resolved UE functions not available on stripped Linux binaries.
- **C++ Mods**: C++ mods must be compiled as `.so` files (not `.dll`).
- **Case Sensitivity**: Linux filesystems are case-sensitive — mod directories must use lowercase `scripts`.

## Troubleshooting

### Server won't start

Check that `LD_PRELOAD` points to the correct absolute path of `libUE4SS.so`.

### Mods not loading

- Verify `mods.txt` exists in the `Mods/` directory
- Verify each mod has a `scripts/main.lua` file (lowercase `scripts`)
- Check the server console output for `[UE4SS]` log messages

### Mod crashes

UE4SS has per-mod crash recovery. If a mod crashes, you will see:
```
[UE4SS] Caught signal 11 during mod execution, recovering...
[UE4SS] Recovered from signal 11 during mod execution, continuing to next mod.
[UE4SS] Mod 'ModName' crashed during startup, continuing to next mod.
```
Other mods will continue to load normally.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for the full changelog.

## Downloads

Download the latest build from the [Releases page](https://github.com/XarminaEu/ue4ss-linux/releases/latest). Old releases are replaced with each new build.
