# Code layout lists (`UA_RELAYOUT`)

One file per libunity build, named by its GNU build id: the functions `relayprof make` found hot in
recordings of a game, as offsets, sizes, weights and hot ranges in that library (nothing of the library
itself). ua_host (AArch64) looks here (`relayout/` beside it) when `UA_RELAYOUT` is unset; a library with
another build id finds no list and runs as it is. How to make one: docs/porting.md, "A code layout list".

| build id | player | made from |
|---|---|---|
| 8936b587837543a476339d9900708f6dc9a2d7e6 | 6000.0.61f1 IL2CPP arm64-v8a (the Mono route's player) | Hollow Knight: King's Pass and Greenpath (time samples, UnityMain + render thread), King's Pass (i-cache stall samples), RG34XX-SP muOS; whole functions (a `--split` list measured better at King's Pass but less well in a scene it was not made from) |
