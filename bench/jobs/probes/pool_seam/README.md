The pool seam's probe (KR-3 step 2, before commit 3's code): what the brief left open about workers that C starts and nobody joins.
`run.sh` builds `kama.json` and runs every mode `csrc/seam.c` lists, on the host, in the Linux container (native, ASan with
LeakSanitizer, TSan) and on wasm under node. `import_cost.sh` asks what a pool file in `std::concurrent` costs a program that
imports only `Atomic`. Results are in docs/ROADMAP_DETAIL.md, `s6-pools`.
