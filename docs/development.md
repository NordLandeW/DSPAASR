# Developing DSPAASR

DSPAASR uses a custom Direct3D 11 NGX bridge, a same-adapter D3D11/D3D12 FSR bridge and a BepInEx 5 adapter. This guide covers building, packaging and testing the integration. For installation and graphics settings, see the [project README](../README.md) and [settings guide](usage.md).

Run the commands below from the repository root. The managed assembly name, native ABI and `dspaa.mod` configuration identity retain their original names for compatibility.

## Scope

- Native-resolution DLAA and genuine lower-resolution DLSS Super Resolution, with SDK-selected input dimensions and native-resolution output/UI.
- FSR 3.1.5 Native AA and four SR modes, with SDK-selected dimensions/jitter, automatic reactive masks and optional RCAS sharpening. Unity stays on D3D11; copies and GPU fence waits cross to a same-adapter D3D12 queue.
- Explicit preset/model selection, independent from SR Quality/Balanced/Performance modes.
- CNN and Transformer choices must reflect the model actually used. An accepted NGX create/evaluate call alone is not evidence that a deprecated preset was honored.
- Optional FSR/DLSS frame generation uses an experimental presentation path through a standard BepInEx preloader patcher, enabled at startup only when a saved FG backend is selected. API/probe checks alone do not certify game image quality or net performance. See the [presentation interface](native-bridge.md#experimental-presentation-interface).
- No ray reconstruction, driver overrides or save conversion. The distributed runtime does not replace game assemblies.

## Native development

Requires Windows x64, Visual Studio 2022 C++ tools, a Windows SDK, PowerShell 7, CMake 3.25 or later, .NET 8+ SDK and .NET Framework 4.8 reference assemblies. The NGX import library uses the release dynamic MSVC runtime (`/MD`); use the Release preset. NGX GPU probing additionally requires supported NVIDIA hardware and driver. FSR probing requires a compatible hardware D3D12/Shader Model 6.2 adapter, D3D11 shared-fence support and shared texture-format support; it is not vendor-gated.

```powershell
pwsh -File tools/fetch-ngx.ps1 -IncludeDevelopment
pwsh -File tools/fetch-fsr.ps1
pwsh -File tools/fetch-minhook.ps1
pwsh -File tools/fetch-streamline.ps1
cmake --preset windows
cmake --build --preset release
ctest --preset release
```

Dependencies download from pinned upstream commits and release archives and are checked against pinned SHA256 hashes. Vendor runtime authenticity is verified using the applicable NVIDIA/AMD signatures; see [third-party software](third-party.md) for component-specific checks and licenses. Existing unexpected files are not overwritten. Dependencies, build outputs and diagnostic artifacts are ignored by Git. Do not copy proprietary game assemblies into this repository.

The managed plugin uses BepInEx 5 and game reference DLLs supplied by the developer. Pass `BepInExPath` and `DspLibsPath` explicitly. Replace the bracketed placeholders below with absolute paths for your installation; the BepInEx core directory must contain `BepInEx.dll` and `Mono.Cecil.dll`.

```powershell
$gameDirectory = '<absolute-game-directory>'
$bepInExCore = '<absolute-BepInEx-core-directory>'
$gameManaged = Join-Path $gameDirectory 'DSPGAME_Data/Managed'
dotnet build managed/DSPAAMod.csproj -c Release `
    "-p:BepInExPath=$bepInExCore" "-p:DspLibsPath=$gameManaged"
dotnet build preloader/DSPAASR.Preloader.csproj -c Release "-p:BepInExPath=$bepInExCore"
# Release validation uses the original game DLLs, not publicized development references.
./tools/test-game-references.ps1 -GameManagedPath $gameManaged -BepInExPath $bepInExCore
./tools/package.ps1 -GameManagedPath $gameManaged -BepInExPath $bepInExCore
```

Set `GameManagedPath` to your installed game's unmodified `DSPGAME_Data/Managed` directory; `DSP_GAME_MANAGED_PATH` is the environment-variable alternative. Pass the same BepInEx 5 core directory to both scripts; use the minimum supported BepInEx version when checking a release's dependency contract. The game-reference check rebuilds all production C# sources, including the Unity UI adapter, into an isolated `build/game-reference-check/` directory. It rejects publicized references and catches direct access to members that are private in the actual game. Access private members through Harmony/reflection instead. The same entry also rebuilds the standalone preloader against the selected BepInEx/Cecil references, without any game/Unity reference. Packaging always runs these checks and includes both exact checked DLLs; the ordinary development reference directory and build outputs are unchanged. This compiler check does not execute Unity or replace in-game menu acceptance.

Packaging requires Python 3 and Inkscape on the tool search path. It writes a new local `dist/` directory and a Gale/Thunderstore ZIP with root package metadata, changelog, README, 256×256 icon, managed/native DLLs, verified release runtimes and license/notices, plus `patchers/DSPAASR.Preloader.dll`. `SHA256SUMS.json` covers every payload file recursively with archive-relative slash-separated paths. The manager routes the patcher separately from ordinary root/plugin files; do not flatten that entry. It does not deploy, launch, upload or publish anything. Default tests cover preset evidence, WARP texture lifetime/fallback/retirement, CLI errors, verified-download safety, model overrides, Apply/Cancel/Defaults, centered jitter and real managed/native ABI calls. They do not execute Unity or validate the native-menu layout.

### Local development paths

Public sources do not assume an installation directory. For repeated local use, create an untracked `Directory.Build.props` at the repository root and replace the placeholders with absolute paths. This file is ignored by Git:

```xml
<Project>
  <PropertyGroup>
    <BepInExPath Condition="'$(BepInExPath)' == ''">&lt;absolute-BepInEx-core-directory&gt;</BepInExPath>
    <DspLibsPath Condition="'$(DspLibsPath)' == ''">&lt;absolute-development-reference-directory&gt;</DspLibsPath>
    <GameDirectory Condition="'$(GameDirectory)' == ''">&lt;absolute-game-directory&gt;</GameDirectory>
  </PropertyGroup>
</Project>
```

For these defaults, priority is an explicit MSBuild `-p:Name=value` or script `-Name value`, then the same-named process environment variable, then the local file. The empty-value conditions above keep MSBuild's environment values intact. Ordinary `dotnet build` imports the file automatically; `package.ps1` and `test-game-references.ps1` use its BepInEx path, while `game-sandbox.ps1` and `game-early.ps1` use its game directory. Scripts only read literal values: they do not evaluate MSBuild expressions, imports or custom conditions. Use absolute literals without MSBuild percent escapes, one value per property and an unconditional PropertyGroup. Missing values fail with setup guidance rather than selecting a guessed installation.

`GameManagedPath` remains an explicit script argument or `DSP_GAME_MANAGED_PATH` environment value. It is deliberately not inferred from `DspLibsPath`, which may contain development-only publicized references. `test-preloader.ps1` keeps its repository-relative minimum-BepInEx default; pass a BepInEx path explicitly to use another core. Run `pwsh -NoProfile -File tools/test-local-paths.ps1` to check the resolver in disposable fixtures without building or launching the game.

Production managed Release builds map this repository's physical path to `/_/` in compiler outputs, including PDB source records and the PE's PDB reference. Portable symbols remain generated; a debugger inspecting a Release build needs a source-path mapping back to the checkout. Debug builds retain ordinary local source paths and breakpoint behavior. See Microsoft's [PathMap and PdbFile documentation](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/compiler-options/advanced#pathmap).

### Preloader helper checks

After the game-reference build, run the configuration and payload-discovery checks in a fresh PowerShell process:

```powershell
pwsh -NoProfile -File tools/test-preloader.ps1 `
    -Plugin build/game-reference-check/Release/bin/DSPAAMod.dll `
    -Preloader build/game-reference-check/Release/preloader/DSPAASR.Preloader.dll `
    -BepInExPath $bepInExCore -GameManagedPath $gameManaged
```

This suite invokes the production preloader helpers with temporary copies of the selected assemblies. It covers profile-relative payload discovery, duplicate/disabled/mismatched installations, saved backend values and read-only configuration handling. It does not initialize the preloader, load the native DLL or start the game/GPU. The startup receipt and settings transactions are covered by the managed tests; actual early-entry timing and presentation capabilities still require in-game validation.


### Headless DLAA probe

From the project root:

```powershell
New-Item -ItemType Directory -Force artifacts/probe-k | Out-Null
./build/native/Release/ngx-probe.exe ./external/ngx/runtime/dev ./artifacts/probe-k K *> artifacts/probe-k/run.log
```

The probe opens **no window**, creates its own D3D11 device, and renders 32 synthetic jittered frames at 1280×720 with depth, zero static-scene motion vectors, and explicit exposure. It writes a PPM image and logs NGX diagnostics. It does not validate game motion vectors or final image quality. Exit 0 requires valid output and a matching application-controlled preset in the runtime's diagnostics; exit 3 means missing evidence, driver control, or a different observed preset. Log syntax is not a stable NVIDIA query ABI, so unfamiliar output remains unverified rather than being declared successful.

E/F are deprecated CNN models; K is first-generation Transformer, L/M second-generation Transformer. Run the probes on the target GPU/driver configuration to verify the requested preset with the pinned runtime. A successful result applies to that configuration rather than establishing compatibility for every driver/GPU.

`ngx-probe <runtime-directory> <output-directory> K --bridge-switch` exercises the same exported DLL used by the managed plugin. It submits frames from one thread and consumes opaque tokens on another, switching K → F → L → M → E → K → K on one camera key, 32 frames per stage. Each stage checks actual preset diagnostics, finite output, selected D3D11 context-state restoration, retirement and shutdown. The final repeated K also exercises unchanged-configuration execution with a reset request. This is still a synthetic static scene, not an image-quality benchmark.

For the separate SR matrix, run `./tools/run-preset-probes.ps1 -SuperResolution`. It uses the real SDK's optimal sizes for DLAA/Quality/Balanced/Performance/Ultra Performance and recommended K/K/K/M/L, then Quality with manual E/L/M: eight stages, 296 frames, fixed 1280×720 output. It directly renders the synthetic scene at each input resolution, checks actual presets, finite/nonblack output and sampled context state, and writes diagnostic images. It opens no window and changes no driver settings. Like the other probes, this does not validate the game's world render, postprocessing or moving geometry.


No NVIDIA-hardware/driver-dependent test is part of default CTest. Its resource/fallback test uses the Windows WARP software D3D11 device. Do not infer supported hardware or performance from a build/CTest pass. Development runtime output contains NVIDIA's diagnostic watermark; never distribute that DLL.

### Headless FSR Native AA / SR probe

```powershell
./build/native/Release/fsr-probe.exe ./external/fsr-sdk/Kits/FidelityFX/signedbin ./artifacts/fsr-probe
```

This opt-in hardware probe opens no window and does not load NGX. It uses the exported native C ABI to query capability/provider and optimal sizes, submit valid typeless D3D11 resources, switch through Native AA/Quality/Balanced/Performance/Ultra Performance and back, then retire/shut down. It checks finite/nonblack output, selected D3D11 state preservation and D3D12 debug-layer errors when the debug layer is available. Stages alternate depth direction, reactive-mask presence and sharpening; each runs two SDK jitter cycles, with an explicit midpoint history reset. The 640×360 matrix is 358 frames with this pinned provider. PPM images/logs are diagnostic artifacts, not proof of Unity integration, transparency quality, motion-vector coverage or performance. FSR capability deliberately does not infer success from an adapter vendor/name.

### Opt-in presentation probes

`present-probe` opens an invisible window and exercises the real D3D11-facing/D3D12 facade. It checks actual pre-UI attachment pixels rather than a stale logical surface; frame/generation matching; retained-slot backpressure and reuse; exact Final/H/depth/motion transfers despite an inherited rejecting draw predicate; predicate restoration on success and exception; resize/reference-count rules; and retirement. With `<AMD-runtime-directory> <Streamline-runtime-directory>` arguments it also switches FSR → DLSS → FSR → Off using the world-snapshot render event and checks missing/incomplete-input fallback.

`fsr-present-probe <AMD-runtime-directory>` and `sl-present-probe <Streamline-runtime-directory> [diagnostic-directory]` exercise the SDK backends independently. Their inputs contain a spatially varying scene and a nonzero, localized translucent screen panel in Final, with no application UI-alpha plane. They cover three size stages, missing HUDless, Off/re-enable and resource retirement; graphics debug errors fail the run. Run these hardware probes explicitly, serially and with an outer process timeout. They are not default CTest dependencies and do not measure physical display FPS or interpolation pixel fidelity.


### Isolated in-game validation

`tools/game-sandbox.ps1` is an explicit developer workflow, not part of a build, test or package command. Close DSP first and supply a new session directory, a BepInEx 5 core compatible with the installation's Doorstop and an already validated, unpacked package. Match the loader entry-point ABI, not just the BepInEx major version: Doorstop 3 uses `Main`, whereas Doorstop 4 uses `Doorstop.Entrypoint.Start`. A core built for the latter does not load through the former.

Use the explicit `$gameDirectory` and `$bepInExCore` paths from the build setup and select an unpacked, validated package:

```powershell
$packageDirectory = '<absolute-unpacked-package-directory>'
./tools/game-sandbox.ps1 -Action Prepare -SessionDirectory artifacts/game-check `
    -GameDirectory $gameDirectory -CoreDirectory $bepInExCore -PackageDirectory $packageDirectory
# Read the emitted OverrideRoot and explicitly set the game's Configs/path.txt to it.
./tools/game-sandbox.ps1 -Action Run -SessionDirectory artifacts/game-check `
    -GameDirectory $gameDirectory -MaximumSeconds 1200
# Only if the host was interrupted, after closing DSP:
./tools/game-sandbox.ps1 -Action Restore -SessionDirectory artifacts/game-check -GameDirectory $gameDirectory
```

Pass the same `GameDirectory` to Prepare, Run and Restore. Preparation copies the core and plugin into a new private profile and backs up the existing `Configs/path.txt` without changing the game. Run refuses an unredirected data path, an existing game process or modified plugin payload; it requests windowed 1280×720 (the game's own options may override this), records its PID and bounds the session lifetime. In the ordinary mode shown above, the child receives DSP's Steam App ID (`1366540`) without altering the parent environment or Steam configuration. Steam must already be running with access to the game.

The game's own path override isolates its file-based saves, blueprints, achievements and options. It does not isolate Steam account services or Unity PlayerPrefs; use a new in-game Sandbox world, not a personal save. A Steam-free assembly alone also does not separate PlayerPrefs. Before exercising menu/Continue code that records play-version counters, independently verify that a private runtime's player company/product identity does not target the installed game's preferences. File restoration receipts do not establish registry isolation. On exit or timeout, the runner stops only its owned process and restores the original path file byte-for-byte. It refuses to overwrite a concurrent external edit. After an abrupt host termination, retain `sandbox.json` and `path.before` and use Restore rather than copying an entire installation/profile. Logs and receipts remain in the session directory. The default headless `game-sandbox-rollback` test checks this filesystem/refusal contract with disposable fixtures; it never starts the real game.

For Steam-free diagnostics, supply a **separately prepared and validated private runtime copy**, not the normal installation or merely a preloader plugin. Its game assembly must already prevent native Steam client initialization even if Doorstop/BepInEx never starts, and the copy must contain no `steam_api*.dll`. The tool does not produce or distribute this modified runtime. Keep it and all proprietary game files private.

At Prepare, pass `-GameDirectory <private-runtime>` and `-SteamFreeAssemblySha256 <validated-64-digit-SHA256>`. The runtime root must contain `steam-free-runtime.json` with `SourceGameDirectory` identifying the distinct original installation and `SteamFreeAssemblySHA256` matching the actual `DSPGAME_Data/Managed/Assembly-CSharp.dll`. This records the policy in `sandbox.json`; subsequent Run calls use that saved policy without repeating the hash. Missing/changed assemblies, a mismatching receipt, native Steam API libraries, or an offline runtime without a pinned session policy are refused before process creation. This is integrity checking for a trusted, previously validated transformation, not proof that arbitrary supplied code is Steam-free.

Only this validated route removes child `SteamAppId`/`SteamGameId` and sets `DSPAASR_FG_VALIDATION_ROOT` to the session for private diagnostic tooling. Clearing environment variables alone is not Steam isolation. Keep the same private GameDirectory for Prepare/Run/Restore, and verify actual initialization state/API-call counts and process modules during menu/world validation; a successful launch is not frame-generation acceptance.

### Testing with an existing NVIDIA App override

Driver overrides take precedence over application hints and can even replace the requested runtime library. The Mod must not silently claim an overridden selection worked. The optional `driver-profile` utility exists only to isolate the **sibling headless probe executable**, never the game or global profile. It is not run by builds or default tests.

The following optional commands temporarily change the probe's application-specific driver profile. Review this behavior before running them:

```powershell
pwsh -File tools/fetch-nvapi.ps1
cmake --preset windows -DDSPAA_BUILD_DRIVER_TOOL=ON
cmake --build --preset release
# Invoke from PowerShell; relative output paths are rooted in this project.
./tools/run-preset-probes.ps1 -RuntimeVariants dev,rel -IsolateDriverProfile
# Bridge switching on the release runtime (same opt-in):
./tools/run-preset-probes.ps1 -RuntimeVariants rel -BridgeSwitch -IsolateDriverProfile
```

The script exports the original driver database, refuses existing application profiles, creates a private four-setting exception, runs bounded probes, and removes that exception in `finally`. It refuses restoration if the private profile has been changed by somebody else. It never imports an entire old database over concurrent user edits. Logs/results go under `artifacts/`. If the hosting process is forcibly terminated, inspect `driver-profile inspect` and use `driver-profile restore` to remove the owned exception; retain the original export until restoration is confirmed.


## Third-party software

See the [native bridge contract](native-bridge.md) for the managed/native interface and the [third-party notices](third-party.md) for dependency licenses. Project-authored source uses the [MIT License](../LICENSE); the downloaded NVIDIA and AMD SDK/runtime components remain under their separate licenses. The build and packaging scripts do not upload or publish the resulting files.
