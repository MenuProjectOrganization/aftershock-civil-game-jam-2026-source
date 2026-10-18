# Selected City Sample derivatives

Source: **City Sample 5.8, Epic Games**, already acquired through Epic Games Launcher/Fab. No purchase was made for this integration.

- [City Sample listing](https://www.fab.com/listings/4898e707-7855-404b-af0e-a505ee690e68)
- [Epic Content License Agreement](https://www.unrealengine.com/eula/content)
- [Fab license terms](https://www.fab.com/eula)

These assets are **licensed Epic content, not CC0 or original project artwork**. Keep source sharing within this private project's authorized collaborators under the acquisition terms. Distribute game content as part of the cooked project; this repository is not a standalone asset pack. Noncommercial participation does not replace the applicable content license.

Credit: Selected environment meshes and architectural surface textures from City Sample, Epic Games. Adapted for AFTERSHOCK. Epic content remains licensed under its applicable acquisition terms.

## Exact bounded selection

`EXTRACTION_MANIFEST.json` contains the actual native export's 13 packages, source/output SHA-256, package sizes, and complete hard/soft Game dependency inventory. The selection totals **86,534,720 bytes**. It consists of two meshes, two prop materials, and nine textures. All output Game references belong to `/Game/Aftershock/CitySample/`.

- Street bench: original geometry and UVs; original bench material graph with its three texture references retargeted to owned derivatives and sampler types matched to their compression settings.
- Street lamp: original geometry and albedo; a small project shader with authored roughness/metallic values. The unused 8K source normal is omitted. This is not a measured reconstruction of the original material.
- Brick colour/normal/packed surface and concrete colour/normal: project derivatives preserve their source pixel data, use streaming, and cap runtime size at 2048. **MaxTextureSize does not downsample the stored editor source bulk**; concrete retains 4K editor source data.
- Brick roughness comes from the **R channel**, proved through the native source material graph. No concrete packed channel was guessed.

Source meshes keep their native centimetre dimensions: the bench is approximately 2.41 m long and 0.99 m high; the lamp is approximately 7.70 m high. No large City Sample building was squeezed into a ten-metre gameplay parcel. Complete building assemblies and the full 104 GiB sample are outside this integration.

## Production recipe and acceptance

`scripts/ue_extract_citysample_selection.py` runs only in a dedicated `CitySampleExtract` scratch project. Its Content root is a real scratch directory; source top folders can be read-only junctions into the acquired sample. `/Game/Aftershock` must resolve within the scratch project, never into VaultCache. Native duplication, texture policy and retargeted prop materials are saved there; only the verified manifest inventory is staged into this project. The extractor rejects Game dependencies outside that exact inventory and verifies source package hashes are unchanged.

`scripts/ue_author_citysample_facade.py` then wraps the two existing building masters. Shared building-type MIDs choose concrete or brick; wood and utility equipment retain their previous surface. Existing glass, roof, cracks, power, retrofit and nonfacade outputs are preserved. The optional facade defaults off, and missing inputs retain the previous material. Near-camera projection fades from 200 to 600 m; distant city views do not need those detail samples.

Two additional shared HISM batches place lamps and benches at native scale, with caps of 96 and 64 and distance culling. They add no prop Actors, point lights or per-prop ticks. They are cosmetic; they do not invent damage, population or services.

Native generation/import/graph checks are separate from current-source Editor/Game builds, actual RHI screenshots, GPU/memory profiling and packaged/cook acceptance. See the current delivery report for which gates actually ran.
