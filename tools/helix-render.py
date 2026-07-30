#!/usr/bin/env python3
"""Render a .kama file in a REAL Helix and report the colour each token actually received.

Colouring is the one thing about an editor integration that cannot be verified by inspection. A query can
compile, every capture can fire under `tree-sitter query`, and the buffer can still come out monochrome —
because `tree-sitter query` reports every match while the editor applies exactly one, and which one it
applies is a documented-inconsistently, version-dependent property of the editor. Helix 25.07.1 applies the
LAST matching pattern; that fact was established with this script, after the first version of
queries/highlights.scm rendered every identifier as plain default foreground.

Usage:
    tools/helix-render.py <file.kama> [--expect TOKEN=CAPTURECLASS ...]

With no --expect it prints a token/colour table. With --expect it asserts that the named tokens do NOT all
share the default foreground colour and that tokens expected to differ actually do, exiting non-zero
otherwise, so it can be used as a gate rather than read by eye.
"""
import os
import pty
import re
import select
import struct
import sys
import termios
import fcntl
import time

KAMA_BIN_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'build', f'{os.uname().sysname}-{os.uname().machine}',
)


def render(path, cols=140, rows=48, settle=5.0):
    """Run `hx <path>` on a pty and return the raw ANSI byte stream it painted."""
    env = dict(os.environ)
    env['PATH'] = KAMA_BIN_DIR + os.pathsep + env.get('PATH', '')
    env['TERM'] = 'xterm-256color'
    env['COLORTERM'] = 'truecolor'
    pid, fd = pty.fork()
    if pid == 0:
        try:
            os.execvpe('hx', ['hx', path], env)
        finally:
            # execvpe only returns on failure, and after pty.fork() a returning child would fall through
            # into the parent's code and run a second copy of this script.
            os._exit(127)
    # Without a window size Helix paints an empty frame and the capture looks like a failure.
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    os.set_blocking(fd, False)
    buf = b''
    deadline = time.time() + settle
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                buf += os.read(fd, 65536)
            except OSError:
                break
    try:
        os.write(fd, b'\x1b:q!\r')
        time.sleep(0.3)
    except OSError:
        pass
    # Reap without blocking: Helix starts `kama lsp` as a child, and a plain waitpid() can hang waiting on
    # a process group that outlives the editor.
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    for _ in range(50):
        try:
            done, _ = os.waitpid(pid, os.WNOHANG)
            if done:
                break
        except OSError:
            break
        time.sleep(0.05)
    try:
        os.close(fd)
    except OSError:
        pass
    return buf


def painted_rows(raw):
    """Reconstruct the painted screen as rows of (colour, text) runs.

    Helix repositions the cursor with CSI ... H at the start of each row, which is what lets a token be
    identified by WHERE it is rather than only by its spelling — necessary here because the whole point of
    the contextual kind word is that the same spelling means different things on different lines.
    """
    text = raw.decode('utf-8', 'replace')
    parts = re.split(r'(\x1b\[[0-9;]*[A-Za-z])', text)
    colour = None
    row = []
    rows = []
    for p in parts:
        if p.startswith('\x1b['):
            if p.endswith('m'):
                colour = p[2:-1]
            elif p.endswith('H'):
                if row:
                    rows.append(row)
                row = []
        elif p.strip():
            fg = ';'.join(colour.split(';')[:5]) if colour and colour.startswith('38;2') else colour
            row.append((fg, p))
    if row:
        rows.append(row)
    return rows


def colour_of(raw):
    """Map each painted word to the set of foreground colours it was painted with."""
    out = {}
    for row in painted_rows(raw):
        for fg, txt in row:
            for word in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', txt):
                out.setdefault(word, set()).add(fg)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    expects = [a[len('--expect='):] for a in sys.argv[1:] if a.startswith('--expect=')]
    if not args:
        print(__doc__)
        return 2
    raw = render(args[0])
    if len(raw) < 1000:
        print(f'FAIL: Helix painted only {len(raw)} bytes — it did not render', file=sys.stderr)
        return 1
    colours = colour_of(raw)

    # "Default foreground" is whatever plain punctuation was painted with — NOT the most common colour.
    # (Once the queries work, the most common colour is the keyword colour, and a most-common heuristic
    # starts reporting every correctly-coloured keyword as uncoloured.)
    default = None
    for row in painted_rows(raw):
        for fg, txt in row:
            if txt.strip() in ('{', '}', ';', '};'):
                default = fg
                break
        if default:
            break

    # Positional report: the same spelling on two lines is the whole point of a contextual keyword, so
    # showing it per-row is the only way to see whether the two were distinguished.
    src_words = set(re.findall(r'[A-Za-z_][A-Za-z0-9_]*', open(args[0]).read()))
    print(f'{"line":<5} {"token":<18} {"fg colour":<22} note')
    print('-' * 66)
    for row in painted_rows(raw):
        flat = ''.join(t for _, t in row)
        m = re.match(r'\s*(\d+)', flat)
        if not m:
            continue
        lineno = m.group(1)
        for fg, txt in row:
            for word in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', txt):
                if word not in src_words:
                    continue
                note = 'UNCOLOURED (default fg)' if fg == default else ''
                print(f'{lineno:<5} {word:<18} {str(fg):<22} {note}')

    # Assertions are RELATIONAL — "this token on this line has the same/different colour as that one" —
    # because the absolute RGB values belong to the user's theme and would make the gate theme-specific.
    # `--expect=3:value!=1:value` reads "the ordinary identifier must not look like the kind word".
    by_pos = {}
    for row in painted_rows(raw):
        flat = ''.join(t for _, t in row)
        m = re.match(r'\s*(\d+)', flat)
        if not m:
            continue
        for fg, txt in row:
            for word in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', txt):
                by_pos.setdefault(f'{m.group(1)}:{word}', fg)

    rc = 0
    for e in expects:
        for op in ('!=', '=='):
            if op in e:
                lhs, rhs = e.split(op, 1)
                a, b = by_pos.get(lhs.strip()), by_pos.get(rhs.strip())
                if a is None or b is None:
                    missing = lhs if a is None else rhs
                    print(f'FAIL: {missing.strip()!r} never appeared in the render', file=sys.stderr)
                    rc = 1
                elif (op == '==' and a != b) or (op == '!=' and a == b):
                    print(f'FAIL: {lhs.strip()} ({a}) {op} {rhs.strip()} ({b}) does not hold', file=sys.stderr)
                    rc = 1
                else:
                    print(f'ok: {lhs.strip()} {op} {rhs.strip()}')
                break
        else:
            print(f'FAIL: malformed --expect {e!r} (want LINE:TOKEN==LINE:TOKEN or !=)', file=sys.stderr)
            rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main())
