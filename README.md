# UE4SS Linux Native Port

A native Linux build of UE4SS (Unreal Engine 4/5 Scripting System) for dedicated game servers running on Linux. This port enables Lua mod loading and scripting on Linux dedicated servers without requiring Windows.

> ## ⚠️ IMPORTANT: Plugin File Format on Linux
>
> **This is a Linux build. It CANNOT load Windows `.dll` files.** Any mod or plugin
> you install must be provided in the **native Linux formats** listed below. If a mod
> author only ships `.dll` files, that mod **will not work** on this Linux build —
> there is no `.dll`-to-`.so` compatibility layer, emulation, or shim of any kind.
>
> | Mod Type | Windows Format | **Required Linux Format** |
> |----------|-----------------|----------------------------|
> | Lua mods | `scripts/main.lua` | `scripts/main.lua` — **identical, no changes needed** (Lua is script-based and platform-independent) |
> | C++ mods | `dlls/main.dll` | `libs/main.so` — **must be a natively compiled Linux shared object (`.so`), not a `.dll`** |
>
> **Key points to remember:**
> - The mod folder for C++ mods on Linux is called **`libs/`**, not `dlls/` (this is different from Windows!).
> - A C++ mod must be **recompiled from source specifically for Linux** (e.g. with GCC or Clang, producing an ELF `.so` file). Simply renaming a `.dll` to `.so` will **not** work — the internal binary format is completely different.
> - Directory names inside a mod folder are **case-sensitive** on Linux (`scripts`, `libs` — always lowercase), unlike Windows where case does not matter.
> - If your mod only provides a `dlls/` folder with a `.dll` inside, UE4SS will **not detect or load it at all** on Linux — you will need the mod author (or yourself, if you have the source) to produce a Linux `.so` build.
>
> **In short: Lua mods just work as-is. C++ mods must be a Linux-native `.so` file inside a `libs/` folder — never a `.dll`.**

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

## AMP Server Setup

If you run your dedicated server through an AMP control panel, AMP overwrites the
server binary on every game update, which breaks the `LD_PRELOAD` setup. See
[docs/AMP-Setup.md](docs/AMP-Setup.md) for a wrapper-script + cronjob solution
that survives AMP updates.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for the full changelog.

## Downloads

Download the latest build from the [Releases page](https://github.com/XarminaEu/ue4ss-linux/releases/latest). Old releases are replaced with each new build.

## Support This Project

If this Linux port has been useful to you, consider supporting further development:

[![Donate](https://img.shields.io/badge/Donate-Stripe-blueviolet?logo=stripe&logoColor=white)](https://buy.stripe.com/8x26oJ2yv1Z3gRE40T5EY00)

**[💜 Donate via Stripe](https://buy.stripe.com/8x26oJ2yv1Z3gRE40T5EY00)**
