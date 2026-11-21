# Code layout lists (`UA_RELAYOUT`)

One file per libunity build, named by its GNU build id: the functions `relayprof make` found hot in
recordings of a game, as offsets, sizes, weights and hot ranges in that library (nothing of the library
itself). ua_host (AArch64) looks here (`relayout/` beside it) when `UA_RELAYOUT` is unset; a library with
another build id finds no list and runs as it is. How to make one: docs/porting.md, "A code layout list".

| build id | player | made from |
|---|---|---|
| 8936b587837543a476339d9900708f6dc9a2d7e6 | 6000.0.61f1 IL2CPP arm64-v8a (the Mono route's player) | Hollow Knight: King's Pass and Greenpath (time samples, UnityMain + render thread, muOS), King's Pass (i-cache stall samples, muOS), City of Tears and King's Pass again (time and i-cache stall samples, UnityMain + render thread, Knulli and muOS, 2026-11-21); `--split` (hot ranges first, cold after): measured against the whole-function list in those scenes and in the Forgotten Crossroads, which no recording covers (arm64-routes.md, "The render thread's code") |
