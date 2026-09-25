# DSPAASR settings and troubleshooting

## Status and requirements

DSPAASR adds native anti-aliasing controls, NVIDIA DLAA/DLSS and AMD FSR 3.1.5 Native AA/Super Resolution to the game's graphics settings. The tested GPU is an RTX 4070 SUPER; this is not an exhaustive cross-GPU compatibility result. Moving objects, particles and animated materials can show artifacts. Menu layout across UI scales and the complete scene/resize lifecycle require broader testing.

Target: Dyson Sphere Program's Unity 2022.3.62f3c1 Mono build on Windows x64, D3D11 and BepInEx 5.4.x. DLSS/DLAA additionally require a supported NVIDIA RTX GPU and a compatible driver; FSR and the original AA modes do not have that NVIDIA-only requirement. FSR additionally requires D3D12/Shader Model 6.2, D3D11 shared fences and compatible shared texture formats on the same graphics adapter. Unity itself remains on D3D11; the FSR bridge has additional copies and synchronization costs. An unsupported backend/device, missing runtime or unverified preset leaves original game AA available. Other mods changing the same AA controls/render hooks may be incompatible. Hot-unloading the native plugin is not supported; restart the game to remove it.

## Installation and removal

For **Gale**, select the Dyson Sphere Program profile and import the supplied ZIP with Gale's local-mod import feature. Its `manifest.json`, README, changelog, 256×256 icon and runtime files are at the archive root; do not add another parent directory around them. The dependency is `xiaoye97-BepInEx-5.4.17`.

For a manual installation, close the game and place this package's files together in a dedicated `BepInEx/plugins/DSPAASR/` directory. When upgrading an existing installation, replace its previous copy rather than keeping two plugin folders. The package contains `DSPAAMod.dll`, `DSPAANative.dll`, the unmodified **release** `nvngx_dlss.dll`, AMD-signed `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll`, the project's MIT license, complete NVIDIA/AMD licenses/notices and a SHA256 manifest. The assembly filenames retain their original names for compatibility. Do not replace any game assemblies or install the development runtime. BepInEx itself is a prerequisite, not part of this package.

First installation preserves the game's existing AA. To remove the mod, close the game, remove its dedicated plugin directory and optionally remove `BepInEx/config/dspaa.mod.cfg` and `BepInEx/cache/DSPAAMod/`. No save conversion is involved. The original game graphics settings remain normal game settings; choosing a custom AA mode and applying it sets native MSAA to off and keeps the game's FXAA fallback enabled.

## Graphics settings

Open the game's graphics options. The former MSAA/FXAA rows become up to three aligned controls. Inapplicable rows, including their labels, are hidden; later options close the gap without resetting saved choices:

- **AA / Super Resolution**: Off, MSAA, FXAA, TAA, DLSS or FSR. There is no separate FXAA checkbox.
- **Resolution Mode**: DLAA (DLSS) / Native AA (FSR), Quality, Balanced, Performance or Ultra Performance. Shown for DLSS/FSR; the first choice renders at native resolution, while the other modes use the selected SDK's actual lower input dimensions.
- **Configuration**: 2×/4×/8× for MSAA; Recommended, CNN, Transformer K, Transformer L or Transformer M for DLSS. Hidden for Off, FXAA, TAA and FSR; an FSR choice never uses a DLSS model preset.

When graphics settings are opened, DLSS capability is checked on the game's actual rendering device. The DLSS entry is disabled while checking or when unsupported, with an explanation beneath the AA controls. Non-NVIDIA devices and unsupported graphics backends are rejected before NGX is loaded; NVIDIA devices are checked through NGX rather than an RTX model-name list. A driver-update requirement is shown when the runtime reports one. Original AA choices remain available. Existing saved DLSS/model/resolution values are preserved, but rendering uses the game's original AA while DLSS is unavailable. After correcting a runtime/driver problem, restart the game to check again.

FSR is checked independently on the game's actual rendering device, including cross-API resource sharing and FSR 3.1.5 availability. A failed DLSS check leaves a supported FSR entry available. Missing or incompatible FSR components preserve the saved selection and use original game AA as a fallback.

Opening the menu does not alter existing game settings. Explicit AA selections choose one technique rather than retaining an invisible MSAA/FXAA combination. Model selection and resolution mode are independent:

| Resolution mode | Recommended preset | Explicit CNN preset |
|---|---|---|
| DLAA | K | F |
| Quality | K | E |
| Balanced | K | E |
| Performance | M | E |
| Ultra Performance | L | F |

Manual Transformer K/L/M always selects that preset at every resolution mode. K is first-generation Transformer; L/M are second-generation. CNN is deprecated by NVIDIA but was verified on the tested 310.9.1 runtime. These labels are model choices, not an L > M > K quality ranking.

Changes are drafts until the normal **Apply** button is used. **Cancel** discards drafts. The graphics tab's **Defaults** resets the draft to original game AA, Recommended model and DLAA resolution mode; Apply is still required. The selected model and resolution mode are retained when switching between DLSS, FSR and other techniques; FSR ignores but preserves the independent DLSS model choice. Existing `Technique = Dlaa` configurations migrate to DLSS + DLAA, even if a newer resolution key is present; upgrading does not silently enable lower-resolution rendering.

SR uses a genuinely lower-resolution world target, then reconstructs in the temporal-AA slot before native-resolution postprocessing and UI. The adapter handles the game's postprocessing stack, Sun Shafts and translucent-UI blur. It preserves caller-owned screenshot/save-thumbnail render targets on the original path; partial viewports also retain original AA. An unintegrated active image effect after reconstruction causes an explicit original-AA fallback rather than silently bypassing that effect. No global texture mip-bias override is applied. Frame generation, ray reconstruction and a performance overlay are not implemented.

On older TextMesh-based game builds, DLSS/FSR draw the main camera's supported floating navigation text at native output resolution **after temporal reconstruction and before the game's DoF, Bloom and color postprocessing**. Its world position, rotation, generated glyphs and original font material remain unchanged; the glyphs no longer participate in temporal AA. This applies only to active `UISailIndicator` text using the original single-pass `GUI/Text Shader`, not all transparent geometry or world indicator lines. Newer builds using WorldSpace Canvas/Unity UI Text (including Steam build 25503975) remain on their original world-render path; the old TextMesh deferral does not cover them. Other AA modes retain the original draw path. Unknown replacement font shaders stay on their existing path rather than being manually redrawn with unsupported lighting/depth behavior. If the temporal stage is unexpectedly skipped, the adapter draws the deferred text after the stack for that fallback frame and reports an original-AA fallback for following frames.

Applied choices persist in `BepInEx/config/dspaa.mod.cfg`. Manual file edits require a game restart; live UI Apply writes immediately.

| Section / key | Values | Default |
|---|---|---|
| Antialiasing / Technique | Original, Fxaa, Taa, Dlss, Fsr; legacy Dlaa is accepted for migration | Original |
| Antialiasing / Resolution | Dlaa, Quality, Balanced, Performance, UltraPerformance | Dlaa |
| Antialiasing / Model | Recommended, Cnn, TransformerK, TransformerL, TransformerM | Recommended |
| FSR / Sharpness | Finite number from 0 to 1; 0 disables RCAS sharpening; file setting, effective after restart | 0 |
| Diagnostics / CaptureShortcut | BepInEx keyboard shortcut; explicit capture only | F10 + LeftControl + LeftShift |

For example, use `Technique = Dlss`, `Resolution = Quality`, `Model = Recommended` in `[Antialiasing]` for Quality with preset K. Use `Resolution = Dlaa` to keep native resolution.

For FSR, set `Technique = Fsr` and choose the same `Resolution` values. `Resolution = Dlaa` is the shared legacy configuration value for **FSR Native AA**; it does not invoke DLAA/NGX in this backend. Optional `[FSR] Sharpness = 0.2` enables mild sharpening. Opaque color is captured before transparent geometry to generate the SDK's reactive mask; this can help transparency reconstruction but cannot create missing animation motion vectors.

## Verifying the selected model

`BepInEx/LogOutput.log` reports the requested and observed preset when a camera first succeeds. Detailed vendor diagnostics are under `BepInEx/cache/DSPAAMod/ngx.log`. The bridge requires a matching **application-controlled runtime creation log**; it never treats the value saved in the configuration as proof of the actual model.

For FSR, successful camera status reports the runtime-verified FSR version and actual input/output sizes.

NVIDIA App/driver overrides can replace the runtime or preset. A driver-controlled result is reported as a failure instead of silently accepting a different model. This mod **does not modify any driver profile**. If an override interferes, review the game's applicable NVIDIA settings yourself; the developer-only profile tool is exclusively for `ngx-probe.exe`, not the game. After correcting the cause, applying settings again retries the camera.

An error inside the asynchronous native event preserves the current source image (spatially enlarged for SR), which can contain that frame's jitter; the bridge restores this prefill even after a partial NGX output write. Following frames revert to the original AA after the error reaches the main thread. A submission error detected on the main thread can use the game's original TAA immediately. Neither path promises a perfectly filtered failed frame, but neither intentionally emits an uninitialized image.

## Animated detail and opt-in capture

Fast rotating collider geometry can lose detail with several models: CNN was worse in the reported comparison, K/M also showed the problem, and L reduced but did not eliminate it. Static inspection found GPU vertex animation without a dedicated MotionVectors pass in the relevant shader. A stationary-camera capture also found all-zero motion vectors across an animated disc region while the reconstructed detail blurred into rings. This supports missing animation-motion input in that captured region, not complete motion coverage elsewhere or a guarantee that adding vectors alone would fix every model. No per-object animation/shader patch is included, and Recommended has not been silently changed to L.

For a requested diagnostic, select DLSS, hold the camera still on the affected object, then press **left Ctrl + left Shift + F10** once. Nothing is captured automatically. The helper saves two consecutive submissions from one camera under `BepInEx/cache/DSPAAMod/captures/<unique-id>/`: unconverted color/depth/motion/output `.raw` files, per-frame XML metadata and a final `result.xml`. Metadata records dimensions/formats, requested preset, native/Unity frame numbers, jitter/reset, camera pose and projection; correlate the NGX log for the actual model/evaluation. `deferredWorldTextCount` records how many navigation-text renderers were excluded from the scene input: when nonzero, these glyphs are intentionally absent from both captured NGX color/output, since their draw happens afterward in the temporal-resolve destination. Inspect the final game image separately. A successful capture means readback and persistence, not visual correctness.

The same shortcut records `PipelineTrace` entries in `BepInEx/LogOutput.log` for the current and following Unity frames. These include the planet/space context, active cameras/components, postprocessing AA state, temporal-AA projection/resolve entry points, native submissions, latest native completion records and image-effect source/destination sizes. The trace does not depend on reaching DLSS, so a bypassed or disabled pipeline can be distinguished from a submitted reconstruction. A submission is not itself proof of successful evaluation or final visibility; correlate completion records, raw output and the observed game image. The request fails explicitly if no complete consecutive pair was submitted within those two frames; already queued GPU readbacks are allowed to finish.

In very dark HDR space scenes, the runtime's debug text can itself be extremely dim even when NGX evaluation succeeds. Its apparent disappearance is not sufficient evidence that DLSS stopped; use the bounded pipeline trace, native completion and raw output together. This is separate from navigation-glyph thinning in reconstruction, addressed by the deferred world-text path above. The adapter does not change exposure or recommended models to brighten the debug indicator.

One 2560×1440 DLAA pair contains about **169 MiB** of raw data, plus GPU staging memory; capturing can disturb frame timing. Requests cannot overlap. Camera/mode changes or nonconsecutive submissions fail the group explicitly, and render targets remain retained while GPU readbacks are pending. Local captures are not uploaded automatically and may be deleted after investigation.


## Live acceptance checklist

Use a disposable/test save or a session with autosaving disabled, not an unprotected daily save. Record the game/BepInEx/NGX logs and screenshots when diagnosing a failure.

- Original AA still works before enabling DLSS/FSR, and again after disabling them; legacy DLAA remains native after upgrading.
- DLAA/FSR Native AA and all four SR modes report the actual expected input/output dimensions; the world source is low-resolution for SR, without shrinking the HUD or later postprocessing.
- Floating navigation text remains readable, correctly oriented and at its original world position/size in DLAA and SR; entering/leaving flight, hiding the indicator, and switching to TAA do not leave missing or duplicate glyphs. Check Bloom/color response as well as the raw-input exclusion.
- All three rows and later options fit at the actual UI scale; original resolution/Bloom dropdowns and new dropdowns are clickable and not occluded. Apply, Cancel, Defaults, reopening and restarting preserve the intended selections.
- Test the first graphics-menu opening after an English cold start, then a Chinese cold start and live language switching. A successful language switch after the controls were already created does not cover first-time initialization.
- CNN/K/L/M each match the runtime's actual preset, and switching creates/resets the appropriate temporal feature.
- Camera pans, rapid turns, zoom/FOV changes, scene/menu transitions, floating-origin changes and resizing do not keep obsolete history.
- Inspect belts, ships, moving machines, particles, transparency and distant thin structures for missing-object motion, ghosting or smearing. Unity's camera-motion buffer alone does not prove object-motion coverage.
- Confirm color/depth/MV alignment and jitter signs in the actual D3D11 frame; a synthetic static-image probe does not establish this.
- No duplicate legacy TAA resolve runs ahead of NGX/FSR; later postprocessing and native UI remain intact. Switching DLSS ↔ FSR retires the previous feature without retaining its preset/status as the new backend's evidence.
- FSR transparent effects and sharpening respond without black output; check its additional GPU copy/synchronization cost rather than assuming the speed of a native D3D12 engine integration.

## NVIDIA notices

The SDK/runtime are proprietary NVIDIA RTX SDK components, not relicensed by this project. The package includes `NVIDIA-RTX-SDK-LICENSE.txt` verbatim and the full third-party notice text in `NVIDIA-DLSS-NOTICES.txt`. Preserve these files, the runtime signature and `third-party.md`. Public/commercial distribution requires separately resolving NVIDIA's applicable license and notification provisions. The vendor runtime may emit its own NGX telemetry; the mod does not implement a performance telemetry service or send notifications on the owner's behalf.

## AMD notices

Keep the complete `AMD-FSR-SDK-LICENSE.md` and `third-party.md` with the two unmodified AMD-signed DLLs. The SDK has component-specific license terms; the bundled runtime is not relicensed under the mod's MIT license. The complete runtime contains providers that the plugin does not select; distribution of that runtime does not mean the mod enables the hardware-specific ML algorithms.
