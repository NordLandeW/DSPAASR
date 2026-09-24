# Third-party software and distribution

## NVIDIA DLSS / NGX

The SDK dependency is NVIDIA/DLSS release 310.9.1, pinned to [commit 374959484e79a640feaba44c93ac8cfb0a03f5b5](https://github.com/NVIDIA/DLSS/tree/374959484e79a640feaba44c93ac8cfb0a03f5b5). Copyright NVIDIA Corporation and its licensors. NVIDIA, the NVIDIA logo and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and/or other countries.

The SDK and runtime are governed by the [NVIDIA RTX SDK license and supplements](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/LICENSE.txt), downloaded verbatim to `external/ngx/LICENSE.txt`. Read the actual license before use/distribution. This notice does not replace or reinterpret it.

- SDK headers, import library and binaries are downloaded dependencies, not project-authored source and not relicensed by this repository.
- Only the release (`rel`) runtime may be considered for an authorized application/plugin distribution, with all applicable license/notice obligations. A runtime must not be distributed as an independent SDK replacement.
- Development (`dev`) runtimes are for private diagnostics, carry a watermark, and must not appear in a release package.
- Preserve NVIDIA signatures and notices. Do not patch the runtime or bypass its authenticity checks.
- Supplement 4 requires notification before a commercial release, including a plug-in to a commercial application. A free mod is not assumed to be exempt without clarification. Supplement 7 separately governs NVIDIA attribution and marks; attribution in a README alone is not represented as NVIDIA approval or confirmation of full compliance.
- The package includes the full third-party copyright and license notices specified by the NVIDIA DLSS Programming Guide in `NVIDIA-DLSS-NOTICES.txt` (source: `docs/nvidia-dlss-notices.txt`), including the referenced Apache License 2.0 text. Retain this file as well as `NVIDIA-RTX-SDK-LICENSE.txt` when redistributing the runtime.

This software contains source code provided by NVIDIA Corporation.

The repository does not include game assemblies or decompiled game code. Local BepInEx/Unity/game reference assemblies are separate development dependencies and require their respective permissions.

## NVIDIA NVAPI (optional developer tool)

The optional driver-profile diagnostic links [NVIDIA/nvapi](https://github.com/NVIDIA/nvapi/tree/70d337db9186e968eab622f7e786de7e437faf3d), pinned to commit `70d337db9186e968eab622f7e786de7e437faf3d`. The SDK's MIT license and copyright notice are downloaded verbatim to `external/nvapi/License.txt` by `tools/fetch-nvapi.ps1`. Preserve that notice with a distribution of the optional tool. NVAPI does not change the separate proprietary terms of DLSS/NGX.

## AMD FSR

The official FSR SDK 2.3.0 is pinned to [GPUOpen-LibrariesAndSDKs/FidelityFX-SDK commit 60f4ea81909200d8542eca14dccb2628b763a9a3](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/60f4ea81909200d8542eca14dccb2628b763a9a3). The package includes the unmodified AMD-signed `amd_fidelityfx_loader_dx12.dll` (2.3.0.2740) and `amd_fidelityfx_upscaler_dx12.dll` (4.1.1.2740). The latter file/API version is **not** the selected reconstruction algorithm: the bridge explicitly selects and verifies the analytical **3.1.5** provider, not the hardware-specific 4.x ML provider.

The complete [SDK license](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/60f4ea81909200d8542eca14dccb2628b763a9a3/docs/license.md) is downloaded to `external/fsr-sdk/docs/license.md` and included verbatim as `AMD-FSR-SDK-LICENSE.md`. Its default terms and listed per-component exceptions differ. Some analytical source components are MIT-licensed, but this does not make the entire SDK or the distributed runtime MIT. Read and preserve the complete copyright, permission and disclaimer text; binaries remain subject to their binary-redistribution terms. This package supplies the runtime as part of the integrated application plugin, not an independent replacement SDK. Do not modify the vendor binaries or their signatures.

No third-party D3D11 FSR wrapper code is included. The project's same-adapter D3D11/D3D12 transport and adapter are project-authored MIT source. AMD, the AMD Arrow logo and FSR are trademarks of Advanced Micro Devices, Inc.; this community mod is not endorsed by AMD.

## Project code and game artwork

DSPAASR's original source code is provided under the MIT License in the repository and package `LICENSE` file. That license does not relicense NVIDIA/AMD components, game assemblies or game artwork.

The mod icon adapts Dyson Sphere Program's game logo by pixelating its lower-left portion; the original logo remains the property of its respective rights holders. Screenshots contain game imagery owned by its respective rights holders. Neither the icon nor the screenshots imply endorsement by the game's developers or NVIDIA.
