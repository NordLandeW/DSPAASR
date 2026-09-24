# DSPAASR

DSPAASR expands the game's graphics settings with more anti-aliasing and super-resolution options. Alongside MSAA and FXAA, you can select the game's built-in TAA, use NVIDIA DLAA at native resolution, or enable DLSS or analytical AMD FSR 3.1 Super Resolution. FSR also provides native-resolution anti-aliasing.

After installation, open the game's **Graphics** settings to choose your anti-aliasing / super-resolution mode and configuration. DLSS resolution modes and model presets can be selected independently.

Super resolution supports **DLSS and analytical FSR 3.1**. FSR is not restricted to AMD hardware and does not select the hardware-specific ML provider. It requires a compatible D3D12/Shader Model 6.2 device on the same adapter as the game's D3D11 renderer. The screenshots below compare the earlier DLSS/native-AA modes.

![Six-way anti-aliasing and super-resolution comparison](https://raw.githubusercontent.com/NordLandeW/DSPAASR/v1.0.0/docs/images/DSPAASR-comparison.png)

![The same six configurations in reverse order](https://raw.githubusercontent.com/NordLandeW/DSPAASR/v1.0.0/docs/images/DSPAASR-comparison-reversed.png)

![MSAA 8x on the outer quarters and DLAA in the middle half](https://raw.githubusercontent.com/NordLandeW/DSPAASR/v1.0.0/docs/images/DSPAASR-MSAA8x-DLAA-MSAA8x.png)

DLSS, including DLAA, can introduce visual artifacts:

- **Transformer K** may make some particle effects less visible.
- **Transformer M/L** are more demanding on the GPU.
- **All models** may misinterpret some rotating material animations, smearing their detail into what looks like a stationary ring.
- Moving objects may exhibit ghosting.

This mod only provides partial fixes for some of the more noticeable visual issues.

-----------

DSPAASR 扩展了游戏的画质设置，提供更多抗锯齿与超分辨率选项。除了 MSAA 和 FXAA，还可以选择游戏内置的 TAA、原生分辨率下的 NVIDIA DLAA，或启用 DLSS、分析式 AMD FSR 3.1 超分辨率。FSR 也提供原生分辨率抗锯齿。

安装后，进入游戏原生的**画质设置**界面，即可调整抗锯齿／超分辨率选项及其配置。DLSS 的分辨率档位与模型配置可以独立选择。

超分辨率支持 **DLSS 与分析式 FSR 3.1**。FSR 不限定 AMD 显卡，也不选择硬件专属的 ML 实现；需要与游戏 D3D11 渲染器位于同一显卡的兼容 D3D12／Shader Model 6.2 设备。以下截图仍是此前 DLSS／原生抗锯齿模式的对照。

![Six-way anti-aliasing and super-resolution comparison](https://raw.githubusercontent.com/NordLandeW/DSPAASR/v1.0.0/docs/images/DSPAASR-comparison.png)

![The same six configurations in reverse order](https://raw.githubusercontent.com/NordLandeW/DSPAASR/v1.0.0/docs/images/DSPAASR-comparison-reversed.png)

![MSAA 8x on the outer quarters and DLAA in the middle half](https://raw.githubusercontent.com/NordLandeW/DSPAASR/v1.0.0/docs/images/DSPAASR-MSAA8x-DLAA-MSAA8x.png)

DLSS（包括 DLAA 模式）可能引入一些视觉问题：

- **Transformer K** 可能使部分粒子效果变得不明显。
- **Transformer M/L** 对 GPU 性能的要求较高。
- **所有模型**都可能错误处理部分旋转的材质动画，将细节涂抹成静止的圆环。
- 移动物件可能出现拖影。

本模组仅针对部分较明显的视觉问题作出了一部分修复。

## License

DSPAASR's original source code is licensed under the [MIT License](https://github.com/NordLandeW/DSPAASR/blob/v1.0.0/LICENSE). Third-party components and game assets are not covered by that license.

**NVIDIA DLSS / NGX — NVIDIA GeForce RTX™**

This mod uses NVIDIA DLSS / NGX. The bundled `nvngx_dlss.dll` is proprietary software owned by NVIDIA Corporation and its licensors, and is governed by the [NVIDIA RTX SDKs License and its supplement](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/LICENSE.txt), not by the MIT License. The package includes the full NVIDIA license and third-party notices; retain them when redistributing the package.

> This software contains source code provided by NVIDIA Corporation.

NVIDIA, NVIDIA GeForce RTX, and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries. Dyson Sphere Program's name, logo, and game imagery belong to their respective rights holders. The mod icon is a modified version of the game's logo.

**AMD FSR**

The bundled, unmodified AMD-signed FSR loader and upscaler are governed by the [FSR SDK 2.3.0 license](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/60f4ea81909200d8542eca14dccb2628b763a9a3/docs/license.md). Preserve the complete `AMD-FSR-SDK-LICENSE.md` and `third-party.md` supplied with the package. The SDK has component-specific terms; the bundled runtime is not relicensed under this project's MIT license. AMD and FSR are trademarks of Advanced Micro Devices, Inc.

DSPAASR is an independent community mod and is not affiliated with or endorsed by Youthcat Studio, NVIDIA or AMD.
