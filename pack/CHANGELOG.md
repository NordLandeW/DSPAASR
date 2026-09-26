# Changelog

## v1.2.0

- Added NVIDIA DLSS Frame Generation and AMD FSR 3.1 Frame Generation, configured independently of anti-aliasing and super resolution. Available backends and DLSS multipliers follow runtime capability checks.
- Added frame-generation controls and NVIDIA Reflex settings to the graphics menu. Frame generation is off by default; the first activation may require a restart.
- Added a standard BepInEx preloader for the presentation bridge. Starting with frame generation off preserves native presentation for SR-only use, without game-file or boot configuration edits.
- Preserved native-resolution navigation text and its Bloom response while keeping supported text and screen UI out of the HUDless frame-generation input.
- Improved graphics-menu layout, status messages and capability filtering while preserving saved choices for temporarily unavailable modes.
- Allowed compatible external runtime replacements without imposing package hashes at game startup; SDK compatibility and vendor checks still apply.
- Reduced frame-generation input copies and handoff overhead, and scoped Reflex pacing and its frame limiter to the active DLSS presentation backend.

Frame generation adds rendering and presentation costs and can reduce the base frame rate; the net benefit varies by hardware and scene. DLSS Multi Frame Generation and Dynamic mode require reported SDK support and remain unverified in-game on their required hardware.

## v1.1.0

Added AMD FSR 3.1.5 Native AA and Super Resolution, with native-device capability checks, SDK-selected resolution/jitter, automatic reactive masks and optional sharpening. Unity remains on D3D11; FSR uses a synchronized same-adapter D3D12 bridge. DLSS model selection and original AA fallback are preserved.

Fixed repeated warnings when checking camera components that have no image-effect method.

Fixed graphics-menu initialization when the game's localized AA labels omit the MSAA/FXAA abbreviations, including the English interface. Thanks to Verteiron for the fix.

## v1.0.1

Fixed a field-access error that broke the graphics settings menu.

## v1.0.0

Mod released
