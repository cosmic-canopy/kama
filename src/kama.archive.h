// kama.archive.h — the .tar.gz `kama publish` writes: the same entries give the same bytes on every machine.
//
// Why not the system `tar | gzip -n`, which publish used until KR-100. Measured 2026-09-26: the same tar
// compressed by Apple gzip 479 and by GNU gzip 1.12 gives two different streams, and the tar headers carried
// the publisher's user and group names and uid, umask-derived modes, an mtime stamped in LOCAL time, and the
// filesystem's directory order. A registry version is identified by its sha256, so every one of those made
// the same commit publish as different bytes on different machines: a `revision` nobody could check by
// rebuilding it, and a mirror published from another machine tripping the dependency-confusion guard. Here
// every output byte is a function of the entries alone — POSIX ustar headers with a fixed owner, time and
// modes, deflate (RFC 1951) and the gzip frame (RFC 1952) written from the specs, no name, no timestamp.
//
// ⚠️ Changing ANY output byte changes the integrity a rebuild of an already-published version computes.
// tools/check-packages.sh pins one archive's sha256 to hold that down.
#pragma once
#include <string>
#include <vector>

struct ArchiveEntry {
    enum Kind { File, Executable, Symlink };
    std::string path;   // '/'-separated, relative to the archive's root directory
    std::string data;   // the file's bytes, or a symlink's target
    Kind kind = File;
};

// A gzip stream holding a tar of `entries`, in the order given, each under `root/`. No directory entries:
// every tar reader creates a file's parents, and a directory's mode is not the package's business.
std::string kamaTarGz(const std::string& root, const std::vector<ArchiveEntry>& entries);
