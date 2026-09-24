# Developing DSPAASR

DSPAASR uses a custom Direct3D 11 NGX bridge and a BepInEx 5 adapter. This guide covers building, packaging and testing the integration. For installation and graphics settings, see the [project README](../README.md) and [settings guide](usage.md).

Run the commands below from the repository root. The managed assembly name, native ABI and `dspaa.mod` configuration identity retain their original names for compatibility.

## Scope

- Native-resolution DLAA and genuine lower-resolution DLSS Super Resolution, with SDK-selected input dimensions and native-resolution output/UI.
- Explicit preset/model selection, independent from SR Quality/Balanced/Performance modes.
- CNN and Transformer choices must reflect the model actually used. An accepted NGX create/evaluate call alone is not evidence that a deprecated preset was honored.
- No frame generation, ray reconstruction, driver overrides, save edits, or game-file replacement.

## Native development

Requires Windows x64, Visual Studio 2022 C++ tools, a Windows SDK, PowerShell 7, CMake 3.25 or later, .NET 8+ SDK and .NET Framework 4.8 reference assemblies. The NGX import library uses the release dynamic MSVC runtime (`/MD`); use the Release preset. Runtime GPU probing additionally requires supported NVIDIA hardware and driver.

```powershell
pwsh -File tools/fetch-ngx.ps1 -IncludeDevelopment
cmake --preset windows
cmake --build --preset release
ctest --preset release
```

Dependencies download from a pinned NVIDIA commit and are checked against pinned SHA256 hashes; the runtime's NVIDIA Authenticode signature is also verified. Existing unexpected files are not overwritten. Dependencies, build outputs and diagnostic artifacts are ignored by Git. Do not copy proprietary game assemblies into this repository.

The managed plugin uses local BepInEx 5 and game reference DLLs. Defaults are `C:/Game Modding/BepInEx/BepInEx/core` and `C:/Game Modding/DSPlibs`; override `BepInExPath`/`DspLibsPath` MSBuild properties on another workstation:

```powershell
dotnet build managed/DSPAAMod.csproj -c Release
# Example: dotnet build managed/DSPAAMod.csproj -c Release -p:DspLibsPath=C:/Local/GameReferences
./tools/package.ps1
```

Packaging requires Python 3 and Inkscape on the tool search path. It writes a new local `dist/` directory and a flat-root Gale/Thunderstore ZIP: package metadata, changelog, README, 256×256 icon, managed/native DLLs, verified release runtime, license/notices and `SHA256SUMS.json`. It does not deploy, launch, upload or publish anything. Default tests cover preset evidence, WARP texture lifetime/fallback/retirement, CLI errors, verified-download safety, model overrides, Apply/Cancel/Defaults, centered jitter and real managed/native ABI calls. They do not execute Unity or validate the native-menu layout.


### Headless DLAA probe

From the project root:

```powershell
New-Item -ItemType Directory -Force artifacts/probe-k | Out-Null
./build/native/Release/ngx-probe.exe ./external/ngx/runtime/dev ./artifacts/probe-k K *> artifacts/probe-k/run.log
```

The probe opens **no window**, creates its own D3D11 device, and renders 32 synthetic jittered frames at 1280×720 with depth, zero static-scene motion vectors, and explicit exposure. It writes a PPM image and logs NGX diagnostics. It does not validate game motion vectors or final image quality. Exit 0 requires valid output and a matching application-controlled preset in the runtime's diagnostics; exit 3 means missing evidence, driver control, or a different observed preset. Log syntax is not a stable NVIDIA query ABI, so unfamiliar output remains unverified rather than being declared successful.

E/F are deprecated CNN models; K is first-generation Transformer, L/M second-generation Transformer. Both release and development 310.9.1 libraries have passed these native-resolution probes on an RTX 4070 SUPER. This is a compatibility result for that configuration, not a guarantee for every driver/GPU.

`ngx-probe <runtime-directory> <output-directory> K --bridge-switch` exercises the same exported DLL used by the managed plugin. It submits frames from one thread and consumes opaque tokens on another, switching K → F → L → M → E → K → K on one camera key, 32 frames per stage. Each stage checks actual preset diagnostics, finite output, selected D3D11 context-state restoration, retirement and shutdown. The final repeated K also exercises unchanged-configuration execution with a reset request. This is still a synthetic static scene, not an image-quality benchmark.

For the separate SR matrix, run `./tools/run-preset-probes.ps1 -SuperResolution`. It uses the real SDK's optimal sizes for DLAA/Quality/Balanced/Performance/Ultra Performance and recommended K/K/K/M/L, then Quality with manual E/L/M: eight stages, 296 frames, fixed 1280×720 output. It directly renders the synthetic scene at each input resolution, checks actual presets, finite/nonblack output and sampled context state, and writes diagnostic images. It opens no window and changes no driver settings. Like the other probes, this does not validate the game's world render, postprocessing or moving geometry.


No NVIDIA-hardware/driver-dependent test is part of default CTest. Its resource/fallback test uses the Windows WARP software D3D11 device. Do not infer supported hardware or performance from a build/CTest pass. Development runtime output contains NVIDIA's diagnostic watermark; never distribute that DLL.

### Testing with an existing NVIDIA App override

Driver overrides take precedence over application hints and can even replace the requested runtime library. The Mod must not silently claim an overridden selection worked. The optional `driver-profile` utility exists only to isolate the **sibling headless probe executable**, never the game or global profile. It is not run by builds or default tests.

The following optional commands temporarily change the probe's application-specific driver profile. Review this behavior before running them:

```powershell
pwsh -File tools/fetch-nvapi.ps1
cmake --preset windows -DDSPAA_BUILD_DRIVER_TOOL=ON
cmake --build --preset release
# Invoke from PowerShell; relative output paths are rooted in this project.
./tools/run-preset-probes.ps1 -RuntimeVariants dev,rel -IsolateDriverProfile
# Bridge switching on the release runtime (same explicit isolation permission):
./tools/run-preset-probes.ps1 -RuntimeVariants rel -BridgeSwitch -IsolateDriverProfile
```

The script exports the original driver database, refuses existing application profiles, creates a private four-setting exception, runs bounded probes, and removes that exception in `finally`. It refuses restoration if the private profile has been changed by somebody else. It never imports an entire old database over concurrent user edits. Logs/results go under `artifacts/`. If the hosting process is forcibly terminated, inspect `driver-profile inspect` and use `driver-profile restore` to remove the owned exception; retain the original export until restoration is confirmed.


## Third-party software

See the [native bridge contract](native-bridge.md) for the managed/native interface and the [third-party notices](third-party.md) for dependency licenses. Project-authored source uses the [MIT License](../LICENSE); the downloaded NVIDIA runtime/SDK remain under their separate license. The build and packaging scripts do not upload or publish the resulting files.
