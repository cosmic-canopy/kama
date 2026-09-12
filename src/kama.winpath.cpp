// kama.winpath.cpp — see kama.winpath.h for why this is its own translation unit.
#ifdef _WIN32
#include "kama.winpath.h"
#include <windows.h>

// UTF-8 <-> UTF-16, the same two conversions include/kama_os.h makes (kama__wide / kama__utf8). Invalid
// UTF-8 yields an empty wide string and the caller falls back to handing the OS the bytes it was given —
// which is what happened before this file existed, not a new failure.
static std::wstring wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// GetFullPathNameW, sized in two calls. Empty on failure. Normalizes separators and collapses `.`/`..`;
// an input that already carries `\\?\` passes through untouched (measured — windows.md), which is why the
// callers strip that prefix BEFORE coming here.
static std::wstring fullW(const std::wstring& w)
{
    DWORD need = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);       // required size, incl. the NUL
    if (need == 0) return std::wstring();
    std::wstring v((size_t)need, L'\0');
    DWORD got = GetFullPathNameW(w.c_str(), need, &v[0], nullptr);       // excludes the NUL on success
    if (got == 0 || got >= need) return std::wstring();
    v.resize(got);
    return v;
}

static bool isVerbatimBack(const std::string& p) { return p.size() >= 4 && p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\'; }
static bool isVerbatimFwd (const std::string& p) { return p.size() >= 4 && p[0] == '/'  && p[1] == '/'  && p[2] == '?' && p[3] == '/';  }

std::string kama_win_fullpath(const std::string& p)
{
    std::wstring w = wide(p);
    if (w.empty()) return std::string();
    return narrow(fullW(w));
}

std::string kama_win_ospath(const std::string& p)
{
    const bool verbatim = isVerbatimBack(p) || isVerbatimFwd(p);
    // The 248 is MAX_PATH minus the 12 CreateDirectoryW reserves for an 8.3 name — the threshold Rust and
    // Go use, and the one kama_os.h uses, so a program and the compiler that built it agree on it.
    if (p.size() < 248 && !verbatim) return p;
    std::string q = p;
    if (verbatim) {
        q = q.substr(4);
        // `\\?\UNC\server\share\x` (either slash) is `\\server\share\x` in the ordinary spelling.
        if (q.size() >= 4 && (q[0] == 'U' || q[0] == 'u') && (q[1] == 'N' || q[1] == 'n') && (q[2] == 'C' || q[2] == 'c')
            && (q[3] == '\\' || q[3] == '/'))
            q = "\\\\" + q.substr(4);
    }
    std::wstring w = wide(q);
    if (w.empty()) return p;
    std::wstring v = fullW(w);
    if (v.empty()) return p;
    if (v.size() >= 4 && v[0] == L'\\' && v[1] == L'\\' && v[2] == L'?' && v[3] == L'\\') return narrow(v);
    if (v.size() >= 2 && v[0] == L'\\' && v[1] == L'\\') v = L"\\\\?\\UNC\\" + v.substr(2);   // \\srv\share\x
    else                                                 v = L"\\\\?\\" + v;
    return narrow(v);
}

bool kama_win_listdir(const std::string& dir, std::vector<std::string>& names)
{
    std::string pat = kama_win_ospath(dir);
    if (!pat.empty() && pat.back() != '/' && pat.back() != '\\') pat += '\\';
    pat += '*';
    std::wstring w = wide(pat);
    if (w.empty()) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        names.push_back(narrow(n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

std::string kama_win_shortpath(const std::string& p)
{
    bool ascii = true;
    for (unsigned char c : p) if (c >= 0x80) { ascii = false; break; }
    if (ascii && p.size() < 248) return p;
    // Shorten the longest existing prefix; carry the remainder (a not-yet-created file, or nested dirs
    // a later step makes) verbatim behind it.
    std::string head = p, tail;
    for (;;) {
        std::wstring w = wide(kama_win_ospath(head));
        if (!w.empty()) {
            DWORD need = GetShortPathNameW(w.c_str(), nullptr, 0);
            if (need) {
                std::wstring s((size_t)need, L'\0');
                DWORD got = GetShortPathNameW(w.c_str(), &s[0], need);
                if (got && got < need) {
                    s.resize(got);
                    std::string out = narrow(s);
                    // kama_win_ospath may have added `\\?\` for a long input; the alias is short, so drop it
                    // the way absolutePath does, and hand back the driver's `/` spelling.
                    if      (out.rfind("\\\\?\\UNC\\", 0) == 0) out = "\\\\" + out.substr(8);
                    else if (out.rfind("\\\\?\\",      0) == 0) out = out.substr(4);
                    for (char& c : out) if (c == '\\') c = '/';
                    return tail.empty() ? out : out + "/" + tail;
                }
            }
        }
        size_t cut = head.find_last_of("/\\");
        if (cut == std::string::npos || cut == 0) return p;      // nothing shorter exists: give back p
        tail = tail.empty() ? head.substr(cut + 1) : head.substr(cut + 1) + "/" + tail;
        head = head.substr(0, cut);
        if (head.size() == 2 && head[1] == ':') return p;        // down to the drive with no alias found
    }
}

// A directory JUNCTION, made with the filesystem call rather than `cmd /c mklink /J`.
//
// ⚠️ WHY NOT mklink. The installer shelled out to cmd, and cmd is MAX_PATH-bound: a path dependency
// under a 265-character project failed with `The system cannot find the path specified.` / `kama
// install: cannot link dependency`, while the byte-identical project at a short path linked fine
// (measured 2026-09-11). `osp()`'s `\\?\` prefix reaches the CRT/Win32 FILE-CALL edge and can do
// nothing for a command line handed to a shell, so the long-path work never covered this call.
//
// A junction — not a directory symlink — because a symlink needs SeCreateSymbolicLinkPrivilege
// (Developer Mode or elevation), which an ordinary `kama pkg install` cannot require. That is a
// pre-existing decision this keeps; see docs/platforms/windows.md.
//
// ⚠️ Not the 8.3 alias either, which is how `ld`/`ar` are fed (kama_win_shortpath). Two reasons it is
// wrong here: a junction STORES its target, so the reparse point would permanently contain
// `C:\MSYS64~1\…` and every later "which package owns this path?" comparison would see the alias; and
// 8dot3 creation can be disabled per volume, where the helper returns its input unchanged and the fix
// would silently not apply. DeviceIoControl has neither problem, and spawns no process at all.
//
// The substitute name is an NT-namespace path (`\??\C:\…`), which is what the reparse point wants;
// the print name is the display form Explorer and `dir` show.
bool kama_win_make_junction(const std::string& target, const std::string& linkPath)
{
    // mingw-w64 declares neither the struct nor (reliably) the control code, so spell both here. The
    // layout is the documented MOUNT_POINT form of REPARSE_DATA_BUFFER.
    struct MountPointReparse {
        DWORD ReparseTag;
        WORD  ReparseDataLength;
        WORD  Reserved;
        WORD  SubstituteNameOffset;
        WORD  SubstituteNameLength;
        WORD  PrintNameOffset;
        WORD  PrintNameLength;
        WCHAR PathBuffer[1];
    };
    const DWORD kTagMountPoint = 0xA0000003;
    const DWORD kFsctlSetReparsePoint = 0x000900A4;
    const size_t kHeader = 16;                       // everything before PathBuffer

    // The target must be absolute with a drive, in `\`-separated form and free of `\\?\` — the NT
    // prefix we add is `\??\`, a different namespace, and doubling them names nothing.
    std::wstring wt = fullW(wide(target));
    if (wt.empty()) return false;
    if (wt.size() >= 4 && wt[0] == L'\\' && wt[1] == L'\\' && wt[2] == L'?' && wt[3] == L'\\') wt = wt.substr(4);
    while (!wt.empty() && wt[wt.size() - 1] == L'\\') wt.resize(wt.size() - 1);   // no trailing sep
    if (wt.empty()) return false;

    const std::wstring subst = L"\\??\\" + wt;
    const std::wstring print = wt;

    // Replace whatever is there. RemoveDirectoryW on a junction removes the LINK, never the target's
    // contents; on a real non-empty directory it fails, which is the outcome we want.
    const std::wstring wlink = wide(kama_win_ospath(linkPath));
    if (wlink.empty()) return false;
    RemoveDirectoryW(wlink.c_str());

    // A junction is an empty directory carrying a reparse point, so the directory comes first.
    if (!CreateDirectoryW(wlink.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;

    HANDLE h = CreateFileW(wlink.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryW(wlink.c_str()); return false; }

    const size_t names = (subst.size() + 1 + print.size() + 1) * sizeof(WCHAR);
    std::vector<char> buf(kHeader + names, 0);
    MountPointReparse* r = reinterpret_cast<MountPointReparse*>(&buf[0]);
    r->ReparseTag = kTagMountPoint;
    r->Reserved = 0;
    r->ReparseDataLength = (WORD)(8 + names);        // counts from SubstituteNameOffset onward
    r->SubstituteNameOffset = 0;
    r->SubstituteNameLength = (WORD)(subst.size() * sizeof(WCHAR));
    r->PrintNameOffset = (WORD)((subst.size() + 1) * sizeof(WCHAR));
    r->PrintNameLength = (WORD)(print.size() * sizeof(WCHAR));
    memcpy(r->PathBuffer, subst.c_str(), (subst.size() + 1) * sizeof(WCHAR));
    memcpy((char*)r->PathBuffer + r->PrintNameOffset, print.c_str(), (print.size() + 1) * sizeof(WCHAR));

    DWORD ret = 0;
    const BOOL ok = DeviceIoControl(h, kFsctlSetReparsePoint, &buf[0], (DWORD)buf.size(),
                                    nullptr, 0, &ret, nullptr);
    CloseHandle(h);
    if (!ok) { RemoveDirectoryW(wlink.c_str()); return false; }   // leave no empty dir behind
    return true;
}

intptr_t kama_win_spawn_shell(const std::string& line)
{
    std::wstring w = wide(line);
    if (w.empty()) return -1;
    std::vector<wchar_t> buf(w.begin(), w.end());   // CreateProcessW may write into lpCommandLine
    buf.push_back(0);
    STARTUPINFOW si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
    // Handles are inherited so cmd's own complaints reach the same stderr the .bat's did; the compiler's
    // streams are redirected by the line itself, beside the object it produces.
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return -1;
    CloseHandle(pi.hThread);
    return (intptr_t)pi.hProcess;
}

int kama_win_wait(intptr_t h)
{
    HANDLE ph = (HANDLE)h;
    DWORD code = 1;
    if (WaitForSingleObject(ph, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(ph, &code)) code = 1;
    CloseHandle(ph);
    return (int)code;
}
#endif
