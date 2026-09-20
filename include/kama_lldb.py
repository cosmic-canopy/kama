# kama_lldb.py — LLDB data formatters, so kama values inspect as kama values.
#
# kama lowers to C, so a debugger reading the debug info sees the C: a `string` is a three-field struct
# of raw pointer and two sizes, an `Optional<T>` is a tag plus a union, and every field the user declared
# carries the `k_` prefix that keeps it out of reach of the C preprocessor (SPEC § *C names*). None of
# that is what the author wrote. This file teaches LLDB the mapping back.
#
# It SHIPS WITH THE COMPILER, beside kama_runtime.h, on purpose: it encodes the emitter's own spellings
# (`kama_tag`, `kama_u`, `kama_vptr`, the `k_` register, the generic-instance mangle), and every one of
# those moves on a compiler release. Versioned anywhere else it would silently describe a layout the
# binary no longer has — the same staleness the name-map was refused for.
#
#     (lldb) command script import "$(kama demangle --lldb-init-path)"
#
# `kama demangle --lldb-init` prints the whole line. The VS Code extension passes it as an initCommand.
#
# WHAT IS HERE, AND WHAT IS NOT. A formatter owns a value's CHILDREN and SUMMARY, and `frame-format`
# owns how a FRAME is printed — so values and the call stack are both fixable here, with no editor in
# the loop. What is not fixable here is the name of a LOCAL: `frame variable` prints the DWARF name and
# LLDB offers no hook to rewrite it, so `k_near` needs a layer between the editor and the debug adapter
# (`editor/vscode/names.js`), which also upgrades these frames from the lexical rendering below to the
# full one that `kama demangle` can produce.

import lldb

CATEGORY = 'kama'
PRELUDE_SCOPE = 'kama__'   # what qualify() puts in front of every prelude declaration

# Read at most this many bytes for a string summary. A corrupt or uninitialized `kama_len` is a number
# like 14472985476293984456 (measured, stopping at a function's entry before its locals are live), and
# without a cap the summary would try to read it and hang the debugger rather than print a bad value.
_MAX_STRING = 4096


def _strip(name):
    """Exactly one `k_`. The mangling is reversible only because it is exactly one: a field the author
    really did call `k_x` is emitted `k_k_x`, and must come back `k_x`, not `x`."""
    return name[2:] if name.startswith('k_') else name


def _real(valobj):
    """The non-synthetic value. A provider that reads its own object through the synthetic view would
    see the children it is itself producing."""
    return valobj.GetNonSyntheticValue()


def _u(v, field):
    c = v.GetChildMemberWithName(field)
    return c.GetValueAsUnsigned(0) if c.IsValid() else 0


def _s(v, field):
    c = v.GetChildMemberWithName(field)
    return c.GetValueAsSigned(0) if c.IsValid() else 0


# ---------------------------------------------------------------------------------------------------
# string
# ---------------------------------------------------------------------------------------------------

def string_summary(valobj, internal_dict):
    """`kama_string` is {char* kama_data; size_t kama_len; size_t kama_cap;}.

    ⚠️ Read EXACTLY `kama_len` bytes. `kama_cap == 0` means the bytes are BORROWED — a literal in static
    storage or a slice of another string — and a borrowed run is defined by its length alone. Reading to
    a NUL would run off the end of the view into whatever static data follows it and print that too.
    Heap-owned strings happen to be NUL-terminated, but honouring the length is correct for both, so
    there is one path here and not two."""
    v = _real(valobj)
    data = _u(v, 'kama_data')
    n = _u(v, 'kama_len')
    if n == 0:
        return '""'
    if data == 0:
        return '<len %d, data NULL>' % n
    take = min(n, _MAX_STRING)
    err = lldb.SBError()
    buf = v.GetProcess().ReadMemory(data, take, err)
    if err.Fail() or buf is None:
        return '<unreadable at 0x%x, len %d>' % (data, n)
    text = buf.decode('utf-8', 'replace')
    return '"%s%s"' % (text, '...' if take < n else '')


class StringSynth:
    """No children. The summary IS the string; `data`/`len`/`cap` are the lowering, not the value."""

    def __init__(self, valobj, internal_dict):
        self.v = _real(valobj)

    def update(self):
        return False

    def has_children(self):
        return False

    def num_children(self, max_children=None):
        return 0

    def get_child_index(self, name):
        return -1


# ---------------------------------------------------------------------------------------------------
# tagged enums: Optional<T>, Result<T, E>, and every user `type enum` with a payload
# ---------------------------------------------------------------------------------------------------

def _variant(v):
    """-> (variant name as kama spells it, payload SBValue or None).

    The lowering is `{ <Inst>_Tag kama_tag; union { struct {...} k_<Variant>; ... } kama_u; }`. The tag
    CONSTANT is spelled `<Inst>_<Variant>`, so the variant name is the constant with the type's own name
    cut off the front. A payloadless variant contributes NO union member at all, so the arm lookup must
    be allowed to miss."""
    # A POINTER to a tagged enum reaches here too — the emitter hoists one for every `match` subject
    # (`kama_msub1`), and user code passes them. LLDB resolves a member through a pointer, so the tag
    # reads fine, but the TYPE NAME is `kama__Optional_string *`, and the variant name is derived by
    # cutting the type's name off the front of the tag constant — which then cut nothing and printed the
    # raw `kama__Optional_string_Some`. Dereference first so both halves see the same type.
    t = v.GetType()
    if t.IsPointerType():
        v = v.Dereference()
        if not v.IsValid():
            return None, None
    tag = v.GetChildMemberWithName('kama_tag')
    if not tag.IsValid():
        return None, None
    label = tag.GetValue() or ''
    tyname = v.GetType().GetUnqualifiedType().GetName() or ''
    name = label[len(tyname) + 1:] if label.startswith(tyname + '_') else label
    u = v.GetChildMemberWithName('kama_u')
    if not u.IsValid():
        return name, None
    arm = u.GetChildMemberWithName('k_' + name)
    if arm.IsValid() and arm.GetNumChildren() > 0:
        return name, arm
    return name, None


def _brief(val):
    """How a payload reads on the parent's one summary line."""
    if not val.IsValid():
        return '?'
    return val.GetSummary() or val.GetValue() or '{...}'


def variant_summary(valobj, internal_dict):
    v = _real(valobj)
    name, arm = _variant(v)
    if name is None:
        return None                       # not the shape we thought: say nothing rather than lie
    if arm is None:
        return name                       # `None`, or any payloadless case
    kids = [_brief(arm.GetChildAtIndex(i)) for i in range(arm.GetNumChildren())]
    return '%s(%s)' % (name, ', '.join(kids))


class VariantSynth:
    """Children are the payload's fields under their kama names — `value`, not `kama_u.k_Some.k_value`.
    The dead arms of the union are not children of anything: only the live one is real."""

    def __init__(self, valobj, internal_dict):
        self.v = _real(valobj)
        self.kids = []

    def update(self):
        self.kids = []
        _, arm = _variant(self.v)
        if arm is None:
            return False
        t = arm.GetType()
        for i in range(t.GetNumberOfFields()):
            f = t.GetFieldAtIndex(i)
            self.kids.append((_strip(f.GetName() or ''), arm, f.GetOffsetInBytes(), f.GetType()))
        return False

    def has_children(self):
        return bool(self.kids)

    def num_children(self, max_children=None):
        return len(self.kids)

    def get_child_index(self, name):
        for i, k in enumerate(self.kids):
            if k[0] == name:
                return i
        return -1

    def get_child_at_index(self, i):
        if i < 0 or i >= len(self.kids):
            return lldb.SBValue()
        name, parent, off, ty = self.kids[i]
        return parent.CreateChildAtOffset(name, off, ty)


# ---------------------------------------------------------------------------------------------------
# contiguous sequences: DynamicArray<T>, FixedArray<T>, View<T>, ConstView<T>
# ---------------------------------------------------------------------------------------------------

class SeqSynth:
    """All four are a pointer plus a count; only the count's NAME differs (`k_len` / `k_size`)."""

    _COUNT = ('k_len', 'k_size')

    def __init__(self, valobj, internal_dict):
        self.v = _real(valobj)
        self.n = 0
        self.data = None
        self.elem = None
        self.sz = 0

    def update(self):
        self.n = 0
        self.data = None
        d = self.v.GetChildMemberWithName('k_data')
        if not d.IsValid():
            return False
        for f in self._COUNT:
            c = self.v.GetChildMemberWithName(f)
            if c.IsValid():
                self.n = c.GetValueAsSigned(0)
                break
        self.elem = d.GetType().GetPointeeType()
        self.sz = self.elem.GetByteSize()
        # A local that is not live yet holds whatever was on the stack. Refusing to believe a negative
        # or absurd count is what keeps the debugger responsive at a function's entry.
        if self.n < 0 or self.sz == 0 or d.GetValueAsUnsigned(0) == 0 or self.n > 1 << 20:
            self.n = 0
        else:
            self.data = d
        return False

    def has_children(self):
        return True

    def num_children(self, max_children=None):
        return self.n if max_children is None else min(self.n, max_children)

    def get_child_index(self, name):
        try:
            return int(name.strip('[]'))
        except ValueError:
            return -1

    def get_child_at_index(self, i):
        if self.data is None or i < 0 or i >= self.n:
            return lldb.SBValue()
        return self.data.CreateChildAtOffset('[%d]' % i, i * self.sz, self.elem)


def seq_summary(valobj, internal_dict):
    v = _real(valobj)
    for f in ('k_len', 'k_size'):
        c = v.GetChildMemberWithName(f)
        if c.IsValid():
            return 'len=%d' % c.GetValueAsSigned(0)
    return None


# ---------------------------------------------------------------------------------------------------
# Map<K, V, H, A> — open addressing with a parallel state byte per slot
# ---------------------------------------------------------------------------------------------------

class MapSynth:
    """`{K* k_keys; V* k_vals; uint8* k_state; isize k_len, k_cap, k_tombs; uint32 k_mods; A k_alloc;}`.

    Driven by `k_cap`, not `k_len`: the live entries are scattered across the slots, so the only way to
    find them is to walk every slot and keep the FULL ones. `k_state[i]` is 0 EMPTY / 1 FULL / 2 TOMB
    (lib/std/collections/map.kama)."""

    _FULL = 1

    def __init__(self, valobj, internal_dict):
        self.v = _real(valobj)
        self.slots = []

    def update(self):
        self.slots = []
        keys = self.v.GetChildMemberWithName('k_keys')
        vals = self.v.GetChildMemberWithName('k_vals')
        state = self.v.GetChildMemberWithName('k_state')
        if not (keys.IsValid() and vals.IsValid() and state.IsValid()):
            return False
        cap = _s(self.v, 'k_cap')
        base = state.GetValueAsUnsigned(0)
        if cap <= 0 or cap > 1 << 22 or base == 0 or keys.GetValueAsUnsigned(0) == 0:
            return False
        err = lldb.SBError()
        buf = self.v.GetProcess().ReadMemory(base, cap, err)
        if err.Fail() or buf is None:
            return False
        self.kt = keys.GetType().GetPointeeType()
        self.vt = vals.GetType().GetPointeeType()
        self.ks, self.vs = self.kt.GetByteSize(), self.vt.GetByteSize()
        self.keys, self.vals = keys, vals
        for i in range(cap):
            if buf[i] == self._FULL:
                self.slots.append(i)
        return False

    def has_children(self):
        return True

    def num_children(self, max_children=None):
        n = len(self.slots)
        return n if max_children is None else min(n, max_children)

    def get_child_index(self, name):
        try:
            return int(name.strip('[]'))
        except ValueError:
            return -1

    def get_child_at_index(self, i):
        if i < 0 or i >= len(self.slots):
            return lldb.SBValue()
        slot = self.slots[i]
        # The key names the child, so a map reads as `["ab"] = 3` rather than by slot index — which is
        # what a reader is actually looking for, and the slot number means nothing to them.
        k = self.keys.CreateChildAtOffset('[%d].key' % i, slot * self.ks, self.kt)
        label = k.GetSummary() or k.GetValue() or ('[%d]' % i)
        return self.vals.CreateChildAtOffset('[%s]' % label, slot * self.vs, self.vt)


def map_summary(valobj, internal_dict):
    v = _real(valobj)
    if not v.GetChildMemberWithName('k_state').IsValid():
        return None
    return 'len=%d' % _s(v, 'k_len')


# ---------------------------------------------------------------------------------------------------
# Owned<T> / Shared<T> / Weak<T>
# ---------------------------------------------------------------------------------------------------

def _is_weak(v):
    name = v.GetType().GetUnqualifiedType().GetName() or ''
    return name.startswith('std__memory__Weak_')


def _strong_of(v):
    """The strong count, or -1 when there is no control block (an `Owned` has none)."""
    c = v.GetChildMemberWithName('k_c')
    if not c.IsValid() or c.GetValueAsUnsigned(0) == 0:
        return -1
    return _u(c.Dereference(), 'k_strong')


class PtrSynth:
    """One child, the pointee, named `*`. `Owned` is `{T* k_p; A k_alloc;}`; `Shared`/`Weak` add
    `Ctrl* k_c` with `{size_t k_strong; size_t k_weak;}`. The allocator is a zero-size field for the
    default and carries no information a reader wants, so it is not a child."""

    def __init__(self, valobj, internal_dict):
        self.v = _real(valobj)
        self.p = None

    def update(self):
        self.p = None
        p = self.v.GetChildMemberWithName('k_p')
        if not p.IsValid() or p.GetValueAsUnsigned(0) == 0:
            return False
        # ⚠️ A `Weak` MUST NOT show its pointee unless a strong reference still holds it. Weak is the one
        # handle whose pointer legitimately outlives the object: once the strong count reaches 0 the
        # pointee is freed and `k_p` dangles, so rendering it would print freed memory as though it were
        # a live value — the most misleading thing a debugger can do while someone is chasing a lifetime
        # bug, which is the only reason to be looking at a Weak.
        if _is_weak(self.v) and _strong_of(self.v) == 0:
            return False
        self.p = p
        return False

    def has_children(self):
        return self.p is not None

    def num_children(self, max_children=None):
        return 1 if self.p is not None else 0

    def get_child_index(self, name):
        return 0 if name == '*' else -1

    def get_child_at_index(self, i):
        if i != 0 or self.p is None:
            return lldb.SBValue()
        return self.p.CreateChildAtOffset('*', 0, self.p.GetType().GetPointeeType())


def ptr_summary(valobj, internal_dict):
    """The refcounts are the whole reason to look at a `Shared` in a debugger — a leak is a strong count
    that did not come down — so they go on the summary line rather than behind an expansion."""
    v = _real(valobj)
    p = v.GetChildMemberWithName('k_p')
    if not p.IsValid():
        return None
    if p.GetValueAsUnsigned(0) == 0:
        return 'null'
    c = v.GetChildMemberWithName('k_c')
    if c.IsValid() and c.GetValueAsUnsigned(0) != 0:
        ctrl = c.Dereference()
        strong = _u(ctrl, 'k_strong')
        counts = 'strong=%d weak=%d' % (strong, _u(ctrl, 'k_weak'))
        # Say so, rather than leave a reader to infer it from a missing child.
        if _is_weak(v) and strong == 0:
            return counts + ' (expired)'
        return counts
    return '0x%x' % p.GetValueAsUnsigned(0)


# ---------------------------------------------------------------------------------------------------
# every other kama type: fields under their kama names
# ---------------------------------------------------------------------------------------------------

class StructSynth:
    """A user `value`/`resource`/`view`, and any kama type without a provider of its own.

    Two compiler-owned members need handling rather than hiding:
      `kama_vptr` — the vtable pointer. It has no kama spelling at all, so it is dropped.
      `kama_base` — the base subobject. Its fields ARE fields of this type as the source declares it,
                    so it is FLATTENED in place rather than hidden; hiding it would make every inherited
                    field unreachable in the debugger."""

    def __init__(self, valobj, internal_dict):
        self.v = _real(valobj)
        self.kids = []

    def update(self):
        self.kids = []
        self._walk(self.v.GetType(), 0)
        return False

    def _walk(self, t, base):
        if t.IsPointerType():
            t = t.GetPointeeType()
        for i in range(t.GetNumberOfFields()):
            f = t.GetFieldAtIndex(i)
            name = f.GetName() or ''
            off = base + f.GetOffsetInBytes()
            if name == 'kama_base':
                self._walk(f.GetType(), off)
                continue
            if name.startswith('kama_'):
                continue                      # kama_vptr and any other synthesized member
            self.kids.append((_strip(name), off, f.GetType()))

    def has_children(self):
        return bool(self.kids)

    def num_children(self, max_children=None):
        return len(self.kids)

    def get_child_index(self, name):
        for i, k in enumerate(self.kids):
            if k[0] == name:
                return i
        return -1

    def get_child_at_index(self, i):
        if i < 0 or i >= len(self.kids):
            return lldb.SBValue()
        name, off, ty = self.kids[i]
        return self.v.CreateChildAtOffset(name, off, ty)


# ---------------------------------------------------------------------------------------------------
# frame names — the CALL STACK, without an editor
# ---------------------------------------------------------------------------------------------------
#
# A frame's name is the C symbol out of the debug info, so a backtrace reads `k_Fapp__Pair_int32__make`.
# LLDB demangles C++ and Rust itself because it knows those schemes; it cannot know kama's. But
# `frame-format` accepts `${script.frame:<module>.<fn>}`, which calls back here per frame — so this is
# fixable WITHOUT an editor in the loop, and a plain `lldb`, a terminal `bt` and any editor that is not
# VS Code all get readable frames from it.
#
# ⚠️ It is LEXICAL, not the front end. It cannot render a generic instance's arguments — `Pair_int32`
# rather than `Pair<int32>` — because the argument spellings and the dropped defaults live in the
# resolved program, which only `kama demangle` has. An editor that runs the name layer upgrades these
# to the full rendering; this is the floor, not the ceiling.
#
# ⚠️ It must leave EVERY non-kama frame exactly as it found it. This hook replaces
# `${function.name-with-args}` in the format string, so it is asked about libc, dyld and any other
# language in the process too.

def _demangle_frame(name):
    """The lexical half of `kama demangle`, for a symbol. Mirrors names.js `lexical()`."""
    if not name:
        return name
    if name == 'kama_main':
        return 'main'
    if name.startswith('k_F'):                      # a file-private scope: `k_F<stem>__<rest>`
        cut = name.find('__')
        if cut > 0:
            return name[cut + 2:].replace('__', '::')
        return name
    if name.startswith(PRELUDE_SCOPE):              # the prelude's scope is implicit in source
        return name[len(PRELUDE_SCOPE):].replace('__', '::')
    if '__' in name:                                # a module path
        return name.replace('__', '::')
    return name                                     # not kama's: hand it back untouched


def frame_name(frame, internal_dict):
    name = frame.GetFunctionName() or ''
    shown = _demangle_frame(name)
    if shown == name:
        return name                                 # not ours (or nothing to do): default rendering
    args = []
    for v in frame.get_arguments():
        val = v.GetSummary() or v.GetValue()
        args.append('%s=%s' % (_strip(v.GetName() or '?'), val) if val else _strip(v.GetName() or '?'))
    return '%s(%s)' % (shown, ', '.join(args))


# ---------------------------------------------------------------------------------------------------
# registration
# ---------------------------------------------------------------------------------------------------
#
# ⚠️ THE MATCH IS A REGEX OVER THE MANGLED TYPE NAME, and there are FOUR shapes, not one. `qualify()`
# spells a type by where it lives:
#
#     prelude                      kama__Optional_int32
#     a loose file (no kama.json)  k_Fapp__Pair_int32          <- the F5 case
#     a module, exported           std__collections__DynamicArray_int32_kama__GlobalAllocator
#     a module, not exported       std__collections__k_Fdynamic_array__Ctrl
#
# so matching `^k_` alone would catch only loose files and miss every stdlib type. A generic instance
# mints its own C type per instantiation, which is why this is a regex family and not a type list.

def _add(debugger, kind, regex, fn):
    debugger.HandleCommand('type %s add -w %s -x "%s" %s' % (kind, CATEGORY, regex, fn))


def __lldb_init_module(debugger, internal_dict):
    m = __name__
    # ⚠️ NO `--language c`, though the emitted code is C and that is what it was written as first.
    # A language-scoped category is only consulted for values LLDB attributes to that language, and it
    # did not attribute these: with `--language c` every provider below registered, the category listed
    # as enabled, and NOTHING applied — `frame variable` printed raw structs. Verified by moving one
    # summary into the default category, where it took effect immediately. An unscoped category is
    # consulted for every value, which is what these regexes already discriminate on.
    debugger.HandleCommand('type category define ' + CATEGORY)

    # string — before everything else, since `kama_string` also matches the compiler-scope family below.
    _add(debugger, 'summary',   r'^kama_string$', '-F %s.string_summary' % m)
    _add(debugger, 'synthetic', r'^kama_string$', '-l %s.StringSynth' % m)

    # Tagged enums. `-e` on the summary so the payload still EXPANDS underneath it: a summary otherwise
    # replaces the children rather than heading them, and `Some(...)` alone would make the payload
    # unreachable in a variables pane.
    variant = r'^kama__(Optional|Result)_.*$'
    _add(debugger, 'summary',   variant, '-e -F %s.variant_summary' % m)
    _add(debugger, 'synthetic', variant, '-l %s.VariantSynth' % m)

    # Contiguous sequences — same `-e`, and here it is the whole point: `len=2` with no elements under
    # it is strictly worse than the raw struct it replaced.
    seq = r'^std__collections__(DynamicArray|FixedArray|View|ConstView)_.*$'
    _add(debugger, 'summary',   seq, '-e -F %s.seq_summary' % m)
    _add(debugger, 'synthetic', seq, '-l %s.SeqSynth' % m)

    mp = r'^std__collections__Map_.*$'
    _add(debugger, 'summary',   mp, '-e -F %s.map_summary' % m)
    _add(debugger, 'synthetic', mp, '-l %s.MapSynth' % m)

    ptr = r'^std__memory__(Owned|Shared|Weak)_.*$'
    _add(debugger, 'summary',   ptr, '-e -F %s.ptr_summary' % m)
    _add(debugger, 'synthetic', ptr, '-l %s.PtrSynth' % m)

    # Everything else kama minted, in each of the four scope shapes. LAST, so the specific providers
    # above win: LLDB prefers the most recently added match, so registration order here is reversed
    # relative to precedence and these must come after.
    for shape in (r'^k_F[A-Za-z0-9_]+__.*$',          # a loose file's own types
                  r'^kama__[A-Za-z0-9_]+.*$'):        # the prelude's
        _add(debugger, 'synthetic', shape, '-l %s.StructSynth' % m)

    debugger.HandleCommand('type category enable ' + CATEGORY)

    # FRAME NAMES. This is LLDB's own default frame-format with `${function.name-with-args}` — and only
    # that — swapped for the hook above, so every other part of a backtrace line (the index, the pc, the
    # module, the file:line, the [opt]/[inlined]/[artificial] markers) renders exactly as it always did.
    # Copied from `settings show frame-format` rather than hand-written, because anything hand-written
    # here silently degrades a backtrace for every language in the process, not just kama's.
    debugger.HandleCommand(
        'settings set frame-format "frame #${frame.index}: '
        '{${ansi.fg.cyan}${frame.pc}${ansi.normal} }{${module.file.basename}{`}}'
        '{${script.frame:' + __name__ + '.frame_name}{${frame.no-debug}${function.pc-offset}}}'
        '{ at ${ansi.fg.cyan}${line.file.basename}${ansi.normal}:${ansi.fg.yellow}${line.number}'
        '${ansi.normal}{:${ansi.fg.yellow}${line.column}${ansi.normal}}}${frame.kind}'
        '{${function.is-optimized} [opt]}{${function.is-inlined} [inlined]}'
        '{${frame.is-artificial} [artificial]}\n"')

    print('kama: value formatters loaded')
