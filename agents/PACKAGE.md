# Working on a kama package

The rules in `AGENTS.md` apply to every kama project. This file is the half that only a **library that
will be published** needs — the manifest floor, the test program, a vendored C library, the C seam, and
publishing. `kama agents install` writes it beside `AGENTS.md` when the manifest says `"kind": "library"`,
and rewrites it on `--force`; put project-specific notes in a file of your own and point at it from here.
Reference: `docs/packages.md` (the manifest, the registry, publishing) and `docs/targets.md`
(`csources`, `cincludes`, build settings).

## The manifest floor

Declare the oldest compiler the package builds with: `"kama": ">=0.9.230"` in `kama.json`. A consumer on
an older compiler is refused by name instead of failing on a construct it does not have. Raise it in the
same commit that starts using a newer feature.

## `tests/` is one program

Make `tests/` its own executable project — `tests/kama.json` with `"kind": "executable"`, an `entry`, and
the package as a **path dependency** (`"@scope/name": { "path": ".." }`) — and drive it from a script
(`tools/test.sh`) that builds it **debug and release** and runs it; a failing case prints its name. A
wasm leg (`--target WASM`, run under node in a container with emcc) is the other half of the gate for any
package that vendors C: a change to `src/` or `csrc/` is not proven until both pass. Test vectors come from
the upstream library's own test files, extracted by script — never typed from memory.

## Vendoring a C library

Vendor it rather than link a system copy: a consumer then needs nothing installed on any target, a
dependency cannot add a machine path like `-I/opt/homebrew/include` for its consumer, wasm has no
system libraries at all, and a shipped binary links one known version. One script owns the vendored tree:

- `tools/vendor-<lib>.sh` holds `VERSION`, `SHA256` and the tarball URL; it verifies the hash or dies,
  extracts, copies the sources pristine into `third_party/<lib>/`, and **regenerates the `csources`
  block of `kama.json` between two marker comments**. Nothing under `third_party/` and nothing in that
  block is hand-edited; upgrading is a change to the two constants, a run, a read of the diff, and a
  version bump of the package.
- `csources` compiles C (`-std=gnu11`, the C the world writes), C++ (`gnu++17`, with `cxxflags`) and
  Objective-C by extension; assembly files are never selected, so the library's C paths are what run. `cincludes` puts the vendored include tree on every
  consumer's path.
- The library's configuration is a `-D` list in the manifest's `cflags`. ⚠️ A dependency's `cflags`
  reach **every translation unit of the consumer's build** — that is kama's design — so the list holds
  only autoconf-style `HAVE_*` names with no plausible collision.
- The package's own C (`csrc/`) is glue only: a once-guard over the library's init, NULL-passing
  wrappers for calls with optional pointers, and a `_Static_assert` per size the kama side spells as a
  literal, so an upgrade that changes one fails at compile time with the name.

## The C seam is spelled from the header

kama emits no prototype for an `extern fn`; the header's is the only one, and the C compiler never sees
kama's declaration. So an input the library declares `const unsigned char *` is an `UnsafeConstPtr<uint8>`,
an output stays `UnsafePtr<uint8>`, and a C string is `UnsafeConstPtr<cchar>` — read the header for every
one, because getting the read-only direction wrong is **silent** (`uint8_t *` into a `const uint8_t *` is
legal C). The extern-agreement rule does catch one symbol declared two ways across files. A byte-holder
exposes its bytes through a `const fn` returning `UnsafeConstPtr<uint8>`, and a caller's `ConstView<uint8>`
window types straight through to a `const` input.

## Private helpers across modules

A member is private to its **type**; a helper in another module that must fill a type's private bytes
gets a `friend` grant (`friend other::mod::Helper::build[bytes];`), never a `public`. A grant reaches
across modules by qualified path with no import, and a grant into a module a given consumer never imports
is inert — so a root type may name every sibling that touches it.

## Publishing

`kama publish kama.json --registry <dir-or-file-uri>` writes an immutable version into a registry — a
directory (or `file://` URI) you then serve or push, not an upload endpoint — and a re-publish of the same
version is refused; the tarball excludes `.git/`, `.kama/`, `out/` and `kama.lock`. Before the first publish:
`--license mit` at seed time (or `"license"` in the manifest and a `LICENSE` file), a README that says
what is vendored and why, and the `tests/` gate green on every target the package claims.
