# Changelog

## v1.1.0

Added AMD FSR 3.1.5 Native AA and Super Resolution, with native-device capability checks, SDK-selected resolution/jitter, automatic reactive masks and optional sharpening. Unity remains on D3D11; FSR uses a synchronized same-adapter D3D12 bridge. DLSS model selection and original AA fallback are preserved.

Fixed repeated warnings when checking camera components that have no image-effect method.

Fixed graphics-menu initialization when the game's localized AA labels omit the MSAA/FXAA abbreviations, including the English interface. Thanks to Verteiron for the fix.

## v1.0.1

Fixed a field-access error that broke the graphics settings menu.

## v1.0.0

Mod released
