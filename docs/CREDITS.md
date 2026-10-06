# Credits

This fork builds on [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) and [OptiScaler](https://github.com/optiscaler/OptiScaler). OptiScaler began with [PotatoOfDoom's CyberFSR2](https://github.com/PotatoOfDoom/CyberFSR2).

Colour processing derives from [clshortfuse's RenoDX](https://github.com/clshortfuse/renodx); see [attribution/licence](../Licenses/RenoDX_ATTRIBUTION.txt).

Integrations include hhkbble's multipass/composition, [y4my4my4m's Vulkan work](NR-VULKAN.md) and [cmh1448's motion metadata](NR-MOTION-METADATA.md). Linked notes identify source commits and test limits.

This variant restores the built-in [Ada MFG work and attribution](RTX40-MFG.md), derived from y4my4my4m and the earlier fork under GPL-3.0.

## OptiScaler contributors

- @PotatoOfDoom for CyberFSR2.
- @Artur for DLSS Enabler and help with the NVNGX API.
- @LukeFZ and @Nukem for their mods and shared knowledge.
- @FakeMichau for support, testing and features.
- @QM for testing and access to games.
- @TheRazerMD for testing and support.
- @Cryio, @krispy, @krisshietala, @Lordubuntu, @scz and @Veeqo for the earlier compatibility matrix.
- The DLSS2FSR community for its support.

This project uses [FreeType](https://gitlab.freedesktop.org/freetype/freetype), licensed under the [FTL](https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/docs/FTL.TXT). Other notices are in [Licenses](../Licenses).

## Upstream sponsorship

Upstream credits [SignPath.io](https://signpath.io/) for Windows code signing and the [SignPath Foundation](https://signpath.org/) for its certificate.

Support upstream: [cdozdil](https://github.com/sponsors/cdozdil?frequency=one-time) and [nitec](https://buymeacoffee.com/nitec).

## MFGAdaUnlock-RenoDx integration (0.2.0)

The quality and input-validation helpers are adapted from [mavismmg/MFGAdaUnlock-RenoDx](https://github.com/mavismmg/MFGAdaUnlock-RenoDx), upstream commit `a2ca96f3eacebf0bec5e25f07f15b46fbf3a0aef`, authored by ImDreamt under MIT. License retained in [MFGAdaUnlock-RenoDx-MIT.txt](../Licenses/MFGAdaUnlock-RenoDx-MIT.txt) and alongside the vendored headers.

The upstream project credits dashdogy for temporal correction and Matias Lombo for Blackwell/Ada kernel rebuild research. Generated CUDA payloads come from locally installed NVIDIA providers; original NVIDIA DLSS/Streamline DLLs are not included in this FG-only archive. Integration scope, fallback and test limits: [MAVIS-INTEGRATION-20261006.md](MAVIS-INTEGRATION-20261006.md).
