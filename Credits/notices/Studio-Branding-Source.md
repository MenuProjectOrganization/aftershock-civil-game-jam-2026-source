# Cyber Security Studios — source and reproduction

Source: [owner-supplied white SVG](https://metucyber.org/assets/img/logo-white.svg), retrieved over HTTPS on 18 October 2026. Exact original: `logo-white.original.svg`, SHA256 `506cc6350fe2a1972bdc9ea92ce92e325254fd81deddc9dae6359e6d9aeb7f09`.

## Final wording

Original eagle; upper **METU NCC**; middle **CYBER SECURITY**; bottom **STUDIOS**; small adjacent **gaming for today**. This follows the owner's latest correction and supersedes earlier wording variants.

The original eagle, star, METU NCC and CYBER SECURITY vector subtrees are preserved byte for byte under XML serialization. Only SOCIETY is replaced. STUDIOS reuses original S/T/U/I/O outlines and an original project-authored D matched to the existing cap height/stem. The small tagline uses outlined glyphs from unmodified **Bitstream Vera Sans Regular**. Its source TTF and complete copyright/permission notice accompany the files in `fonts/`; the final SVG has no text nodes, font loading or raster references.

The original logo is used under the project owner's supplied-artwork/edit authorization. No CC0 or general logo/trademark redistribution permission is inferred. The font was copied from bundled ReportLab `fonts/Vera.ttf`; its [official upstream archive](https://download.gnome.org/sources/ttf-bitstream-vera/1.10/) and included license permit redistribution with this software. No font file was modified or sold separately. No technology/company icons were added.

## Deliverables

- `cyber-security-studios-white.svg`: outlined vector master.
- `cyber-security-studios-white-2048.png`: 2048×403 transparent RGBA master for Unreal UI.
- `cyber-security-studios-white-1024.png`: 1024×201 transparent derivative.
- `splash-cyber-security-studios-{1920x1080,1280x720}.bmp`: black, optically centred RGB24 native splash candidates, no alpha.
- `preview-*.png`: inspection images, **not game screenshots**.

White RGB extends through transparent pixels; alpha retains antialiased edge coverage. BMPs use BI_RGB and explicit LCS_sRGB in the [108-byte BITMAPV4HEADER](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapv4header). Pillow independently decodes every BMP and compares all RGB pixels with its PNG preview. Source, generated images, font, license and generator hashes are in `SOURCE_AND_BUILD_MANIFEST.json`.

## Reproduction

Prerequisites: Python with Pillow and FontTools 4.67.0; Node.js and Sharp. The CPU generators do not install dependencies or access the network. FontTools can be installed normally or into the ignored repository `.tools/branding-python` directory. The source TTF is included.

```powershell
python _Imports/Branding/MetuCyberStudios/generate_logo.py --node 'path/to/node.exe' --sharp-module 'path/to/node_modules/sharp'
python scripts/ue_import_studio_branding.py --preflight
```

The generator uses fourfold supersampling and two CPU workers; its manifest fails closed on errors. Root coordinates Unreal import, splash copy, runtime startup, hard reference/cooking and build validation. No native acceptance is inferred from source preparation. The importer targets only `/Game/UI/Branding/T_CyberSecurityStudios`; it does not copy a BMP or edit Config.
