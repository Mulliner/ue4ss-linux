# Blueprint Modloader

> **Linux Status:** Blueprint mod loading is included in the Linux build but has significant limitations:
> - It requires UE function addresses to be resolved (GUObjectArray, ProcessInternal, etc.)
> - On **stripped Linux binaries** (most dedicated server builds), these symbols cannot be found via `dlsym`, so blueprint mod loading **will not work**
> - On **unstripped binaries** or when manual address overrides are provided, blueprint mod loading can work
> - The following hooks must be enabled in `UE4SS-settings.ini`:
>   - `HookLoadMap=true`
>   - `HookBeginPlay=true`
>   - `HookProcessInternal=true`
> - Blueprint `.pak` files must be placed in `Content/Paks/LogicMods/`

As our BP system is based on RussellJ's, this tutorial video is applicable for creating a blueprint mod for UE4SS:

<iframe width="560" height="315" src="https://www.youtube.com/embed/fB3yT85XhVA?si=Hvu0wHmcFxFT2MA_" title="YouTube video player" frameborder="0" allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share" allowfullscreen></iframe>