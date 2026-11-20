# How big is real game and editor state, and what is it made of?

2026-11-15. Every number has a link. "Estimate" means my own arithmetic or judgement, not a sourced figure.

## Short answer

Most of a shipped game's bytes are textures and audio, not the kind of data our format serializes. In real engines meshes are also not reflected structs: they are cooked into GPU-ready blobs (binary data laid out the way the GPU reads it) and loaded as raw bytes. The best primary figure for "reflected object" data is Horizon Zero Dawn: 16 million objects across 300,000 source files, shipped as 20 GiB of object files beside 15 GiB of streamed bulk data ([Guerrilla](https://guerrilla-games.com/read/Streaming-the-World-of-Horizon-Zero-Dawn)). That is a whole game in about 189,000 files, not one file. A single large open-world level in an editor has hundreds of thousands of actors, so roughly 0.1–1 GB of entity state (estimate). A save game is 1–100 MB, with forum reports of a few hundred MB. A streamed world cell is a few MB. So our 1 GB, 1-million-entity file is the top of the realistic range for one file. 10 GB of entity state in one file has no real-world counterpart I could find. Our 27% mesh share matches no real case: world data's bulk arrays are instance transforms and height fields, and real mesh data is an opaque blob.

## 1. What a shipped AAA game is made of

There is no systematic public breakdown. These are the best data points:

| Game | What's known | Source |
|---|---|---|
| Horizon Zero Dawn (Decima engine) | 189,000 object ("Core") files = 20 GiB; 30,000 streamed bulk ("CoreStream") files = 15 GiB; 1.2 GiB of localized data per language | [Guerrilla slides](https://guerrilla-games.com/read/Streaming-the-World-of-Horizon-Zero-Dawn) |
| Titanfall (PC) | 35 GB of a 48 GB install was uncompressed audio | [PC Gamer](https://pcgamer.com/titanfall-install-audio) |
| Ashes of the Singularity | Textures were about 60% of the install (Stardock) | [PC Gamer](https://www.pcgamer.com/why-are-game-install-sizes-getting-so-big) (from a search summary; I could not load the page) |
| Wolfenstein: The New Order (id Tech 5) | 33 GB in the `virtualtextures` folder alone | [Tom's Guide forum](https://forums.tomsguide.com/threads/wolfenstein-the-new-order-read-this-before-playing.17820/latest) (forum post) |
| Unreal Engine 5 guidance | Textures 40–70%, meshes 10–20%, audio 5–15% of content | [Stray Spark blog](https://www.strayspark.studio/blog/ue5-packaging-shipping-build-size-optimization) (rule of thumb, not measured) |
| Cyberpunk 2077: Phantom Liberty | 3,000–4,000 animations, streamed within a 40 MB budget | [GDC 2024 slides](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Tremblay_Charles_Anatomy_of_a.pdf) |

What I infer: textures and audio are usually 60–85% of the install, meshes 10–20%, and level, scene and gameplay object data a few percent (estimate). Horizon is the exception that proves the point. Its "Core" files are reflected objects (C++ classes with attributes and links, converted from a text format to binary), and they are a large share of the game. But Core also holds any bulk data that isn't streamed, so 20 GiB is an upper bound on object data, not a measurement of it.

## 2. Level, world and scene data

| Example | Size or count | Source |
|---|---|---|
| Horizon Zero Dawn, whole game | 16M objects, 20M links between objects, 300k source files | [Guerrilla](https://guerrilla-games.com/read/Streaming-the-World-of-Horizon-Zero-Dawn) |
| Unreal World Partition map, 20 × 15 km (a studio's project) | ~300k actors, 9k always loaded; editor load took 1.5–3 minutes | [Unreal forum](https://forums.unrealengine.com/t/map-load-times-in-editor/2552578) |
| The Matrix Awakens (UE5 city demo) | ~7,000 buildings, 45,073 parked cars, 260 km of road, 1,248 intersections | [Epic](https://www.unrealengine.com/en-US/wakeup) |
| Cyberpunk 2077 | Over 26,300 streaming sectors (world chunks, each a tree of placed nodes) | [RED modding wiki](https://wiki.redmodding.org/cyberpunk-2077-modding/for-mod-creators-theory/files-and-what-they-do/the-whole-world-.streamingsector) |
| Ghost of Tsushima | A 200 × 200 m terrain tile is ~2 MB compressed on disc, including terrain and all foliage placements | [PSU, quoting Sucker Punch's lead engine programmer](https://psu.com/news/sucker-punch-talks-super-fast-load-times-for-ghost-of-tsushima) |
| Ghost of Yōtei | Distant mountains hold over 1M trees, rocks and bushes, culled to ~60k drawn | [PlayStation Blog](https://blog.playstation.com/?p=411201) |

How editors store it. Unreal's One File Per Actor saves each placed actor to its own file while editing; a cook folds them back into the level files ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/one-file-per-actor-in-unreal-engine)). Unity scenes and prefabs are YAML text in the editor ([Unity](https://docs.unity.com/en-us/engine/7000.0/manual/working-with-scenes/text-scene-format/format-description.md)). Godot's text `.tscn` becomes binary `.scn` on export ([Godot](https://docs.godotengine.org/en/4.5/engine_details/file_formats/tscn.html)). Large per-instance data, such as foliage, is stored as packed arrays of transforms, not one object per tree. Tsushima fits a whole tile of placements in ~2 MB.

Estimate: average reflected-object size. Horizon gives at most 20 GiB / 16M ≈ 1.3 KB per object. Our benchmark is ~730 MB of small structs for 1.07M entities plus materials and clips, so ~700 B per entity. The two are the same order of magnitude.

Not found: a measured byte size for a big Unreal `__ExternalActors__` folder or a large Unity scene. Nobody publishes Content-folder splits for big projects.

## 3. Save games

| Game | Size | Source |
|---|---|---|
| Cyberpunk 2077 | Saves corrupted once they reached ~8 MB; the support advice was to carry fewer items and crafting materials | [Gfinity, quoting GOG support](https://www.gfinityesports.com/article/cyberpunk-2077-players-reporting-save-files-being-corrupted-xbox-ps4-ps5-series-x-s-pc) |
| Baldur's Gate 3 | Larian removed the save size cap because some players' story databases outgrew it; a PC Gamer writer had over 3 GB of saves by Act 2, across many files | [PC Gamer](https://www.pcgamer.com/larians-had-to-patch-baldurs-gate-3s-save-files-to-be-infinite-size-to-account-for-the-sheer-amount-of-things-youre-all-doing-on-your-journeys) |
| Starfield | ~3.4 MB for a new game; a player reports 73 MB after much exploration | [Steam forum](https://steamcommunity.com/app/1716740/discussions/0/3955910099428042172) (player report, from a search summary) |
| Skyrim | Normal saves grow from ~5 MB to a few tens of MB; players report trouble from 55–60 MB | [Nexus forum](https://forums.nexusmods.com/topic/867224-safe-size-of-skyrim-saves-before-its-unplayable) (anecdotes) |
| Factorio | A 96 MB save held 2.1M entities; another 91 MB save was ~39 MB entities and ~50 MB tiles | [Factorio forum](https://forums.factorio.com/viewtopic.php?p=242624) (forum figures from a search summary; the site blocked my fetch) |

Structure: the Bethesda games save "change forms", meaning only what differs from the shipped world (moved, inventory changed and so on), compressed with zlib or LZ4 ([UESP](https://en.uesp.net/wiki/Skyrim_Mod:Save_File_Format)). That delta design is why saves are megabytes while worlds are gigabytes. Simulation games like Factorio save the full state, including big tile arrays.

## 4. Meshes

- Nanite meshes in Epic's Valley of the Ancient sample average 14.4 bytes per input triangle, so ~13.8 MB per million triangles. A 1.5M-triangle mesh is 148.95 MB as a classic static mesh with 4 LODs, but 19.64 MB with Nanite ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-technical-details)).
- Classic compressed index data costs ~1–1.2 bytes per triangle, and vertex compression gives 2–4× over packed quantized vertices ([meshoptimizer](https://github.com/zeux/meshoptimizer)).
- Storage is a cooked, engine-specific blob, compressed and quantized. Horizon ships streamed bulk data as separate CoreStream files ([Guerrilla](https://guerrilla-games.com/read/Streaming-the-World-of-Horizon-Zero-Dawn)). No engine I found loads vertex data field by field through reflection. The reflected part is a small header (bounds, material slots, LOD settings) that points at the blob.

Not found: sourced per-character or per-scene vertex counts for current AAA games. What I found was forum posts and vendor guides.

## 5. How engines serialize this today, and how fast

- **Unreal.** In the editor, properties are "tagged": each value carries its name and type so old data still loads after a class changes. Cooked builds use unversioned property serialization, which is faster, assumes the reader's layout matches the writer's, and cannot upgrade old data ([Epic API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FArchiveState/SetUseUnversionedPropertySeriali-), [Epic staff on the forum](https://forums.unrealengine.com/t/older-pak-files-fail-to-deserialize-when-loaded-in-new-version-of-the-project/2723767)). The Zen loader works out the package dependency graph offline ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/zen-loader-in-unreal-engine)). Streaming spends a set number of milliseconds per frame on async loading and actor setup ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/streaming-settings-of-the-unreal-engine-project-settings)).
- **Unity.** Binary SerializedFiles carry an optional type tree, a schema of fields and types. Editor files keep it; standalone builds usually drop it ([disunity wiki](https://github.com/ata4/disunity/wiki/Serialized-file-format)). Scene activation time grows linearly with GameObject count ([Meta](https://developers.meta.com/horizon/blog/avoiding-hitches-when-loading-scenes-in-unity/)). The ECS package bakes subscenes into binary entity files ([Unity](https://docs.unity3d.com/Packages/com.unity.entities@0.51/api/Unity.Scenes.Editor.EditorEntityScenes.html)).
- **Decima.** Text source objects are converted to binary per file. Loading runs deserialize, resolve links, initialize, activate. Files are packed into 256 KiB compressed blocks, ordered to be read in sequence ([Guerrilla](https://guerrilla-games.com/read/Streaming-the-World-of-Horizon-Zero-Dawn)).
- **Throughput.** The PS5 SSD reads 5.5 GB/s raw and typically 8–9 GB/s after decompression ([GSMArena, Sony specs](https://www.gsmarena.com/sony_details_playstation_5_hardware_features_and_specifications-news-42095.php)). Sony's demo cut Spider-Man fast travel from ~8 s on PS4 Pro to 0.8 s ([Tom's Hardware](https://www.tomshardware.com/news/sony-demos-playstation-5-storage-spider-man,39395.html)). The 300k-actor Unreal map took 90–180 s to open in the editor ([forum](https://forums.unrealengine.com/t/map-load-times-in-editor/2552578)). That time includes far more than parsing, but it shows the scale.

Compare: we load ~130–170 MB/s on one thread (1 GB in 5.8–7.6 s), about 170k entities per second. That is 30–40× slower than the PS5's raw read speed. For streaming, the bar is the per-frame budget, not total throughput.

## 6. Realistic workloads for our format

All sizes below are estimates built from the figures above.

| Scenario | Size per file | Small structs vs bulk arrays | Bulk is | Speed that matters |
|---|---|---|---|---|
| A. Editor saves or loads a large open-world level | 0.1–1 GB, 100k–1M entities (from 300k actors at ~0.5–3 KB each) | ~80–95% small structs | Instance-transform arrays (foliage, props), spline points, height fields | Whole-file load and save time; a second or two counts as good |
| B. Runtime save game | 1–100 MB typical, a few hundred MB at the extreme | ~90%+ small structs (deltas, inventory, quest flags, string IDs); simulation games up to ~50% bulk tiles | Tile or grid arrays | Save in well under a second on a background thread; size limits on consoles |
| C. Streamed world cell, cooked | 0.1–5 MB (Tsushima: ~2 MB for a 200 m tile including terrain) | ~30–60% small structs, rest packed placement arrays | Transforms, instance IDs | Milliseconds per cell, done inside a per-frame budget; many cells each second |

Meshes appear in none of these as reflected data. They live in separate cooked assets and should be one opaque byte array.

How our benchmark compares:

- **Size.** 100 MB–1 GB is realistic for scenario A. 10 GB of entity state in one file is not; keep it only as a stress test and label it so.
- **Bulk content.** Our 27% bulk is vertex data. Real world data's bulk is arrays of transforms and instance records, and meshes are opaque blobs. Swap the mesh arrays for instance-transform arrays, and add a separate "opaque blob" test that only measures pass-through bytes.
- **Missing small-file case.** Scenario C is the runtime hot path: thousands of 0.1–5 MB loads. Add a cell benchmark that reports microseconds per cell and MB/s on small inputs, where fixed per-file costs dominate.
- **Missing save case.** Add a 1–100 MB save made of deltas: sparse changed fields, many short strings, inventories as maps.
- **Entity size.** ~700 B per entity matches Horizon's upper bound of ~1.3 KB per object. Keep it.

## Sources

Most trusted (primary, from the studio or engine maker):
- Guerrilla, Streaming the World of Horizon Zero Dawn (slides): https://guerrilla-games.com/read/Streaming-the-World-of-Horizon-Zero-Dawn
- Epic, Nanite technical details: https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-technical-details
- Epic, One File Per Actor: https://dev.epicgames.com/documentation/en-us/unreal-engine/one-file-per-actor-in-unreal-engine
- Epic, Zen loader: https://dev.epicgames.com/documentation/en-us/unreal-engine/zen-loader-in-unreal-engine
- Epic, streaming settings: https://dev.epicgames.com/documentation/en-us/unreal-engine/streaming-settings-of-the-unreal-engine-project-settings
- Epic, FArchiveState unversioned serialization: https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FArchiveState/SetUseUnversionedPropertySeriali-
- Epic, The Matrix Awakens: https://www.unrealengine.com/en-US/wakeup
- CD Projekt Red, GDC 2024 Phantom Liberty slides: https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Tremblay_Charles_Anatomy_of_a.pdf
- Sony, Ghost of Yōtei tech deep dive: https://blog.playstation.com/?p=411201
- Unity, text scene format: https://docs.unity.com/en-us/engine/7000.0/manual/working-with-scenes/text-scene-format/format-description.md
- Unity, EditorEntityScenes: https://docs.unity3d.com/Packages/com.unity.entities@0.51/api/Unity.Scenes.Editor.EditorEntityScenes.html
- Meta, Avoiding hitches when loading scenes in Unity: https://developers.meta.com/horizon/blog/avoiding-hitches-when-loading-scenes-in-unity/
- Godot, TSCN format: https://docs.godotengine.org/en/4.5/engine_details/file_formats/tscn.html
- meshoptimizer: https://github.com/zeux/meshoptimizer

Press quoting developers:
- PSU (Sucker Punch quote): https://psu.com/news/sucker-punch-talks-super-fast-load-times-for-ghost-of-tsushima
- PC Gamer, Titanfall audio: https://pcgamer.com/titanfall-install-audio
- PC Gamer, install sizes: https://www.pcgamer.com/why-are-game-install-sizes-getting-so-big
- PC Gamer, Baldur's Gate 3 saves: https://www.pcgamer.com/larians-had-to-patch-baldurs-gate-3s-save-files-to-be-infinite-size-to-account-for-the-sheer-amount-of-things-youre-all-doing-on-your-journeys
- Gfinity, Cyberpunk saves: https://www.gfinityesports.com/article/cyberpunk-2077-players-reporting-save-files-being-corrupted-xbox-ps4-ps5-series-x-s-pc
- GSMArena, PS5 specs: https://www.gsmarena.com/sony_details_playstation_5_hardware_features_and_specifications-news-42095.php
- Tom's Hardware, PS5 Spider-Man demo: https://www.tomshardware.com/news/sony-demos-playstation-5-storage-spider-man,39395.html

Community (weaker; anecdotes or reverse engineering):
- Unreal forum, 300k-actor map: https://forums.unrealengine.com/t/map-load-times-in-editor/2552578
- Unreal forum, cooked data is unversioned: https://forums.unrealengine.com/t/older-pak-files-fail-to-deserialize-when-loaded-in-new-version-of-the-project/2723767
- RED modding wiki, streaming sectors: https://wiki.redmodding.org/cyberpunk-2077-modding/for-mod-creators-theory/files-and-what-they-do/the-whole-world-.streamingsector
- UESP, Skyrim save format: https://en.uesp.net/wiki/Skyrim_Mod:Save_File_Format
- disunity wiki, Unity SerializedFile: https://github.com/ata4/disunity/wiki/Serialized-file-format
- Nexus forum, Skyrim save sizes: https://forums.nexusmods.com/topic/867224-safe-size-of-skyrim-saves-before-its-unplayable
- Steam forum, Starfield saves: https://steamcommunity.com/app/1716740/discussions/0/3955910099428042172
- Factorio forum, save contents: https://forums.factorio.com/viewtopic.php?p=242624
- Tom's Guide forum, Wolfenstein virtual textures: https://forums.tomsguide.com/threads/wolfenstein-the-new-order-read-this-before-playing.17820/latest
- Stray Spark, UE5 build size: https://www.strayspark.studio/blog/ue5-packaging-shipping-build-size-optimization
