# AFTERSHOCK — Civil Game Jam 2026 source

Public source-code review export for the competition. Team: Alayköy, four members; representative: Mert Sert (team declaration).

## Exact submission version

Windows jury build source: `d6a6da0243e326ef46d5755f79edc8092a7f425b`.
All tracked `Source/**` and `Config/**` files are byte-for-byte from that development commit. `SOURCE_EXPORT.json` records every exported project file and SHA256, so the public export commit can differ without implying a different game implementation. This is an intentional clean export, not fabricated development history.

The matching playable Windows build is submitted separately through the official competition portal. Unreal Editor is not required to play the cooked ZIP. The public repository is not a downloadable third-party asset pack.

## Engine and compilation

Unreal Engine **5.8.3**, Windows x64; Visual Studio 2022 C++ toolchain and Windows SDK. The submitted build used MSVC14.44 and Windows SDK10.0.22621. Engine/runtime code and binaries are obtained separately under Epic's terms.

Use the engine `GenerateProjectFiles.bat` for `JamProject.uproject`, then build the `JamProjectEditor` Development Win64 target with the engine `Build.bat`. Game target is `JamProject` Shipping Win64. These are instructions, not a fresh build verification of this exported checkout.

## Binary content and regeneration

Binary `Content/**` and original `_Imports/**` media are deliberately absent from this code review export; a fresh clone is **not claimed to be immediately playable or to reproduce the full artwork with one command**. Team-created code and text-based asset authoring scripts remain present. Third-party raw content cannot be blanket relicensed because the competition requires a public code repository.

1. Read `ASSET_SETUP.md` and `Credits/CREDITS.md`; obtain third-party source packs through their official pages under your own entitlement where required.
2. Follow the corresponding import/authoring script inputs and exact `/Game/...` destination paths. `scripts/building_asset_map.json` maps building meshes.
3. Create/import the required maps, textures, sound waves and materials with UE5.8.3. Review script prerequisites and local source paths before running them. Some paths refer to the original development workspace and need local configuration.
4. Inspect asset references in the Editor, then compile/cook. Restricted raw City Sample/ElevenLabs content is not supplied here. This import process was used during development; it is not a validated one-click clone-to-artwork recreation.

## Attribution and AI

`Credits/` contains the existing source/provenance/license notices. Creative code/material/procedural media authoring, design, debugging and documentation used OpenAI ChatGPT/Codex assistance. Team planning also used the team-reported Argon AI workflow; exact additional model names are not independently verified. Sound Effects were generated using ElevenLabs `text_to_sound_v2`; the team reports the Creator plan at generation. See `ELEVENLABS_PLAN_DECLARATION.md` and the official terms.

No source, asset or dataset in this repository is collectively declared CC0/MIT. Original team code remains owned by its rights holders; third-party examples, data and notices retain their respective terms. Competition usage permission is accepted separately by the authorized representative with the team's approval.
