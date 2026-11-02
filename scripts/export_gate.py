"""Static export gate: every public API symbol must be in the shared export table.

Why this gate exists
--------------------
`tests/test_shared_link.cpp` is a *probe*: it proves only that the symbols it
happens to name are exported. Audit B measured the gap
(`.opencode/audit-p0-shared-b.md` section 1.5): a new public class with
out-of-line members but no `CRPC_API` still builds, still exports nothing, and
all 38 suites still pass. A probe cannot close that gap, because "which symbols
must be exported" is a set question, not a test-case question.

What this gate checks
---------------------
The authority for "must be exported" is the header, read with a depth-tracking
scanner (not a line regex: a call expression such as `return endpoint::tcp(...)`
at the start of a line looks exactly like a definition to a line regex, which is
how an earlier version produced 443 false positives on a correct tree).

  A. A non-template class/struct/union **with a body** in `include/crpc/**` that
     declares at least one member **without a body** (so the definition lives in
     `src/`) must carry `CRPC_API`. A header-only type has no such member and is
     out of scope: nothing to export, and nothing the DLL may pin.

  B. A class that carries `CRPC_API` must actually export something. This is the
     "decoration is present but does not take effect" case.

  C. A namespace-scope function declared **without a body** must carry
     `CRPC_API`. Such a declaration promises a symbol that lives in `src/`, and
     an undecorated one is invisible in the export table while still appearing
     in the header a consumer compiles against.

  D. A class template must NOT carry `CRPC_API`: the DLL can export only the
     specialisations it instantiated itself, so a consumer instantiating
     `span<int>` would fail to link. Audit B reproduced exactly that
     (`.opencode/audit-p0-shared-b.md` section 2.2). `tests/test_span_template.cpp`
     is the runtime half: it instantiates specialisations nothing else in the
     tree uses and calls through them, so a decorated template becomes a link
     error there even though the library's own build stays green.

`struct` and `union` default to public access, `class` to private. This tree has
224 `struct` declarations against 122 `class` ones, so normalising the keyword to
`class` (an early bug) made every struct member look private, skipped rule A for
37 structs with out-of-line members, and still printed `OK`. Audit B reproduced
it by removing `CRPC_API` from `struct client_detail::call_core`, which produced
a real `LNK2001` while the gate reported success.

Why the object files are consulted at all
-----------------------------------------
Rule B cannot be decided from the headers: a decoration can be present and still
produce no export. So the gate also reads `dumpbin /exports` and asks whether
any exported name mentions the class. That is the only use of the binaries; the
"must" side is purely static, which is what makes the gate work on Linux and in
review as well as on a Windows build.

Exit codes: 0 = clean, 1 = violation, 2 = the gate could not run (no shared
object, no usable symbol reader, unparsable export table). 2 is never treated as
success: a gate that cannot run must not report green.
"""
import argparse
import io
import os
import re
import subprocess
import sys

# An export row starts with the ordinal, hint and RVA columns; everything after
# them is the symbol name. Two things make a single line insufficient to recover
# the name, both measured on this machine's `dumpbin`:
#
#   * the name is followed by a ` = ` suffix -- either an ILT reference such as
#     ` = @ILT+1234(name)` or an annotation such as ` = name (comment)`;
#   * a long name is wrapped across several lines, so one logical entry spans
#     multiple physical lines until the next entry starts.
#
# `read_exports` therefore accumulates the lines of one logical entry and takes
# the text before the first ` = `. The old line regex
# `^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$` matched neither shape:
# it rejected every row carrying a ` = ` suffix, so a build with `--build-dir`
# parsed zero symbols and the gate exited 2 (could not run) instead of checking
# anything.
_EXPORT_ROW = re.compile(r'^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(.*)$')
# The export table ends at the `Summary` section. Its rows (`1000 .00cfg`) are
# not entries, and a continuation line would otherwise absorb them.
_EXPORT_SECTION = re.compile(r'^\s*[0-9A-Fa-f]+\s+\.\w')

# ---------------------------------------------------------------------------
# Source text handling
# ---------------------------------------------------------------------------

_CLASS_OPEN = re.compile(
    r'\A\s*(template\s*<.*>\s*)?'
    r'(class|struct|union)\s+(CRPC_API\s+)?(?:\[\[[^\]]*\]\]\s*)*'
    r'(\w+)\s*(?:final\s*)?(?:[:]([^{;]*))?\s*\Z', re.S)
_NAMESPACE_OPEN = re.compile(r'\A\s*(?:inline\s+)?namespace\s*([\w:]*)\s*\Z', re.S)
_ACCESS = re.compile(r'\A\s*(public|private|protected)\s*\Z')
_IDENT_BEFORE_PAREN = re.compile(
    r'([A-Za-z_]\w*|~\s*[A-Za-z_]\w*|operator\s*[^\s(]+)\s*\Z')
_TRAILING_QUALIFIERS = re.compile(
    r'(?:\s|^)(?:const|noexcept(?:\s*\([^()]*\))?|override|final|volatile|'
    r'&&|&|=\s*0|=\s*default|=\s*delete)\s*\Z')
_SKIP_KEYWORDS = ('using ', 'typedef ', 'friend ', 'static_assert', 'template',
                  'return ', 'enum ', 'class ', 'struct ', 'union ', 'namespace',
                  'static ', 'extern ', 'constexpr ')


def strip_comments_and_literals(text):
    """Blank out comments and string/char literals, preserving offsets.

    Offsets are preserved so a reported line number still points at the real
    source line. A UTF-8 BOM is replaced by a space for the same reason it must
    not survive: `\\s` does not match U+FEFF, so a BOM at the start of the file
    makes every `\\A`-anchored match fail for the *first* construct in the file.
    Seven headers in this tree have one (`io.hpp`, `observability/log.hpp`,
    `observability/tracer.hpp`, `cluster/{election,lease,watch}.hpp`,
    `mvc/attributes.hpp`); before this was handled, `io::sink` and
    `observability::logger` were indexed as having no out-of-line members,
    which silently exempted them from rule A.
    """
    def blank(match):
        return re.sub(r'[^\n]', ' ', match.group(0))
    if text.startswith('\ufeff'):
        text = ' ' + text[1:]
    text = re.sub(r'//[^\n]*', blank, text)
    text = re.sub(r'/\*.*?\*/', blank, text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', blank, text)
    return re.sub(r"'(?:\\.|[^'\\\n])*'", blank, text)


def strip_directives(text):
    """Blank out preprocessor lines, preserving offsets.

    A directive can be continued with a trailing backslash, and the continuation
    line is not required to start with `#`. `codec.hpp:201-203` has
    `#if __has_include(<rtti/json.hpp>) && \\` followed by more conditions; a
    line-at-a-time match removed only the first line, leaving
    `... && \\` in the text. That fragment then sat between `namespace crpc {`
    and the next `{`, so the namespace frame became an `other` frame and every
    class after it -- including `codec_registry` -- was dropped. Three files in
    this tree use continuations (`codec.hpp`, `mvc/attributes.hpp`,
    `observability/log.hpp`).
    """
    def blank(match):
        return re.sub(r'[^\n]', ' ', match.group(0))
    return re.sub(r'(?m)^[ \t]*#(?:[^\n]*\\[ \t]*\n)*[^\n]*', blank, text)


def function_name(statement):
    """The declared function name in `statement`, or None if it is not one.

    Returns None for the three declarations that end in `;` but are *not*
    out-of-line definitions: `= default`, `= delete` and `= 0` (pure virtual).
    Treating those as "defined in src/" made the first version report
    `crpc::status`, `crpc::metadata_list` and every abstract interface as a
    class with out-of-line members, which is wrong: nothing is defined for them
    anywhere, so there is nothing to export.

    Also returns None for a member with a brace-or-equal initialiser, such as
    `db::mysql::connection_options::capabilities`. That initialiser spans several
    `mask(...)` calls, so the statement the scanner sees is
    `std::uint32_t capabilities = mask(...) | mask(...)`, and the identifier
    before the last parenthesis is `mask` -- a call, not a declaration. The first
    version reported it as an out-of-line member and demanded CRPC_API on a plain
    options struct. The `=` is matched only at the top level of the statement, so
    `std::size_t f(std::size_t x = 1);` (a default argument) still counts.
    """
    stmt = statement.strip()
    if not stmt or stmt.lstrip().startswith(_SKIP_KEYWORDS):
        return None
    # A top-level `=` marks a variable definition with an initialiser (including
    # `constexpr double x = limits::infinity();`) or a pure-virtual / defaulted
    # member, never a declaration whose name is the identifier before the last
    # parenthesis. Matched outside parentheses so a default argument still counts.
    _depth = 0
    for _i, _ch in enumerate(stmt):
        if _ch in '({[':
            _depth += 1
        elif _ch in ')}]':
            _depth -= 1
        elif _ch == '=' and _depth == 0:
            _prev = stmt[_i - 1:_i]
            _next = stmt[_i + 1:_i + 2]
            if _next == '=' or _prev in ('=', '<', '>', '!', '+', '-',
                                         '*', '/', '%', '&', '|', '^'):
                continue  # comparison or compound operator, not an initialiser
            if stmt[:_i].rstrip().endswith('operator'):
                continue  # `operator=`: the `=` is part of the name
            return None
    if re.search(r'=\s*(?:default|delete|0)\s*\Z', stmt):
        return None
    while True:
        trimmed = _TRAILING_QUALIFIERS.sub('', stmt)
        if trimmed == stmt:
            break
        stmt = trimmed
    stmt = stmt.rstrip()
    if not stmt.endswith(')'):
        return None
    depth, open_at = 0, -1
    for i in range(len(stmt) - 1, -1, -1):
        ch = stmt[i]
        if ch == ')':
            depth += 1
        elif ch == '(':
            depth -= 1
            if depth == 0:
                open_at = i
                break
    if open_at < 0:
        return None
    m = _IDENT_BEFORE_PAREN.search(stmt[:open_at])
    if not m:
        return None
    name = m.group(1).replace(' ', '')
    if name in ('if', 'while', 'switch', 'for', 'catch', 'sizeof', 'decltype',
                'noexcept', 'static_assert', 'alignof'):
        return None
    return name


class Frame(object):
    __slots__ = ('kind', 'name', 'is_template', 'api', 'access', 'stmt_start')

    def __init__(self, kind, name='', is_template=False, api=False,
                 stmt_start=0, keyword='class'):
        self.kind = kind
        self.name = name
        self.is_template = is_template
        self.api = api
        # `class` starts private, `struct` and `union` start public. Getting
        # this wrong is silent: every member of every struct would look
        # private, `out_of_line_member` would stay False, and rule A would
        # skip the type while reporting OK. This tree has 224 `struct`
        # declarations against 122 `class` ones, so the bug hid most of the
        # codebase -- audit B reproduced it by removing `CRPC_API` from
        # `struct client_detail::call_core`, which produced a real LNK2001
        # while the gate said OK.
        self.access = 'private' if keyword == 'class' else 'public'
        self.stmt_start = stmt_start


class HeaderIndex(object):
    def __init__(self):
        # Keyed by (scope chain, name): `crpc::span` is a class template while
        # `crpc::obs::span` is an exported class, so a bare-name key would let
        # one shadow the other and silently exempt the template from rule D.
        self.classes = {}
        self.free_decls = {}
        # (scope chain, name) of namespace-scope functions *defined* inline in a
        # header. A forward declaration followed by such a definition needs no
        # CRPC_API: the consumer compiles the body from the header, so there is
        # no DLL copy to import. Rule C used to demand the macro anyway, because
        # a `;` records a free declaration and the later `{ ... }` body never
        # cleared it -- `client_detail::live_owner`, `live_owner_release` and
        # `async::detail::current_executor` were reported in error, and the
        # wrong fix (decorating them) would have exported an inline function
        # that every consumer also defines.
        self.free_defs = set()
        # Count of `{` blocks the scanner could not attribute to a class,
        # namespace or enum. Most are function bodies and initialisers, so the
        # absolute value is not meaningful -- but every historical scanner bug
        # (the BOM, the `#if` continuation, the `struct` default access) moved
        # this number, because a mis-parsed declaration turns into an anonymous
        # `other` frame. Printing it makes that kind of regression visible even
        # when M1/M2/M3 and the free-declaration count happen to cancel out.
        self.other_frames = 0

    def scope_chain(self, stack):
        chain = []
        for frame in reversed(stack):
            if frame.kind == 'other' or not frame.name:
                continue
            for part in frame.name.split('::'):
                if part and part != 'crpc':
                    chain.append(part)
        return tuple(chain)


def scan_headers(include_dir):
    index = HeaderIndex()
    for dirpath, _dirnames, filenames in os.walk(include_dir):
        for filename in sorted(filenames):
            if not filename.endswith('.hpp'):
                continue
            path = os.path.join(dirpath, filename)
            rel = os.path.relpath(path, os.path.dirname(include_dir))
            raw = io.open(path, encoding='utf-8', errors='replace').read()
            text = strip_directives(strip_comments_and_literals(raw))
            regions = conditional_regions(raw)
            before = dict(index.classes)
            _scan(text, rel, index)
            # Tag every class this file introduced with the platform its
            # `#if` region names, so a Linux-only type is not reported as a
            # missing Windows export.
            for key, info in index.classes.items():
                if key in before or info['file'] != rel:
                    continue
                for start, end, platform in regions:
                    if start <= info['line'] <= end:
                        info['platform'] = platform
                        break
    return index


def _scan(text, path, index):
    stack = [Frame('ns', stmt_start=0)]
    line_starts = [0]
    for i, ch in enumerate(text):
        if ch == '\n':
            line_starts.append(i + 1)

    def line_of(offset):
        lo, hi = 0, len(line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if line_starts[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '{':
            head = text[stack[-1].stmt_start:i]
            frame = Frame('other', stmt_start=i + 1)
            m = _CLASS_OPEN.match(head)
            if m:
                # A class declared in a `private:` / `protected:` section of an
                # enclosing class is an implementation detail: its members are
                # still out of line (so the enclosing class needs the macro,
                # which rule A checks), but the nested class itself is not part
                # of the API and must not be reported. `inproc_connection::
                # source_adapter` and `sink_adapter` are exactly this case.
                if any(f.kind == 'class' and f.access != 'public' for f in stack):
                    frame = Frame('other', stmt_start=i + 1)
                else:
                    frame = Frame('class', m.group(4), m.group(1) is not None,
                                  m.group(3) is not None, i + 1,
                                  keyword=m.group(2))
                    key = (index.scope_chain(stack), m.group(4))
                    info = index.classes.setdefault(key, {
                        'name': m.group(4), 'api': False, 'template': False,
                        'file': path, 'line': line_of(i),
                        'out_of_line_member': False, 'platform': None,
                        'bases': []})
                    info['api'] = info['api'] or frame.api
                    if m.group(5):
                        for base in m.group(5).split(','):
                            base = base.strip()
                            base = re.sub(r'\b(?:public|protected|private|virtual)\b',
                                          '', base).strip()
                            if base and '<' not in base:
                                info['bases'].append(base.split('::')[-1].strip())
                    info['template'] = frame.is_template
                    info['file'] = path
                    info['line'] = line_of(i)
            else:
                m = _NAMESPACE_OPEN.match(head)
                if m:
                    frame = Frame('ns', m.group(1), stmt_start=i + 1)
            if frame.kind == 'other':
                index.other_frames += 1
                # A `{` reached at namespace scope after a parameter list is a
                # function body; remember the name so rule C does not treat the
                # earlier declaration as body-less.
                head_name = function_name(head)
                if head_name:
                    index.free_defs.add((index.scope_chain(stack), head_name))
            stack.append(frame)
        elif ch == '}':
            if len(stack) > 1:
                stack.pop()
            stack[-1].stmt_start = i + 1
        elif ch == ';':
            statement = text[stack[-1].stmt_start:i]
            _record(statement, stack, index, path, line_of(i))
            stack[-1].stmt_start = i + 1
        elif ch == ':':
            label = text[stack[-1].stmt_start:i]
            if stack[-1].kind == 'class' and _ACCESS.match(label):
                stack[-1].access = _ACCESS.match(label).group(1)
                stack[-1].stmt_start = i + 1
        i += 1


def _record(statement, stack, index, path, line):
    if not statement.strip():
        return
    if any(f.kind == 'other' for f in stack):
        return
    name = function_name(statement)
    if not name:
        return
    frame = stack[-1]
    api = re.search(r'\bCRPC_API\b', statement) is not None
    if frame.kind == 'class':
        key = (index.scope_chain(stack[:-1]), frame.name)
        info = index.classes.get(key)
        if info is None:
            return
        if frame.access == 'public' and not info['template']:
            info['out_of_line_member'] = True
        return
    # Namespace scope: a declaration without a body.
    key = (index.scope_chain(stack), name)
    prev = index.free_decls.get(key)
    if prev is None or (api and not prev['api']):
        index.free_decls[key] = {'api': api, 'file': path, 'line': line}


# ---------------------------------------------------------------------------
# Export table
# ---------------------------------------------------------------------------

def _export_name(block):
    """The symbol name from one logical export entry.

    `block` is the entry's first line (without the ordinal/hint/RVA columns)
    followed by any wrapped continuation lines. The name ends at the first
    ` = `; everything after it is an ILT reference or a comment and is dropped.
    Continuation lines carry no separator of their own, so they are joined
    directly -- MSVC mangled names contain no spaces, so a space would corrupt a
    wrapped name rather than reconstruct it.
    """
    text = block[0] + ''.join(line.strip() for line in block[1:])
    return text.split(' = ', 1)[0].strip()


def read_exports(dumpbin, dll):
    out = subprocess.run([dumpbin, '/exports', dll],
                         capture_output=True).stdout.decode('utf-8', 'replace')
    lines = [line.rstrip('\r') for line in out.split('\n')]
    try:
        start = next(i for i, line in enumerate(lines)
                     if 'ordinal hint' in line) + 1
    except StopIteration:
        raise RuntimeError('no export table header in %s' % dll)
    names = set()
    block = None
    for line in lines[start:]:
        if line.strip() == 'Summary' or _EXPORT_SECTION.match(line):
            break
        m = _EXPORT_ROW.match(line)
        if m:
            if block is not None:
                names.add(_export_name(block))
            block = [m.group(1)]
        elif block is not None and line.strip():
            # A wrapped continuation of the current entry's name.
            block.append(line)
    if block is not None:
        names.add(_export_name(block))
    if not names:
        raise RuntimeError('no exports parsed from %s' % dll)
    return names


def _run_tool(argv):
    """Run `argv` and return its decoded stdout.

    A missing executable raises `FileNotFoundError`; the caller turns that into
    a skip rather than a crash, because the gate must fall through to the next
    available reader instead of dying on the first absent tool.
    """
    proc = subprocess.run(argv, capture_output=True)
    return proc.stdout.decode('utf-8', 'replace')


def _nm_argv(tool, demangle):
    """`nm` arguments for the dynamic, defined-only table.

    GNU `nm` spells it `-D --defined-only`; macOS spells it `-gU` and does not
    accept GNU's long form, so the whole flag list is chosen per platform.
    """
    flags = ['-gU'] if sys.platform == 'darwin' else ['-D', '--defined-only']
    if demangle:
        flags.append('-C')
    return [tool] + flags


def _parse_nm(text):
    """Dynamic symbols from `nm -D --defined-only` / `... -C`.

    Columns are `address type name`. The demangled name can contain spaces
    (`crpc::http2::plain_free(int)`), so everything after the two leading
    columns is rejoined rather than taking the last token.
    """
    names = set()
    for line in text.split('\n'):
        parts = line.split()
        if len(parts) >= 3 and parts[-1]:
            names.add(' '.join(parts[2:]))
    return names


_READELF_TABLE = re.compile(r"Symbol table '([^']+)'")
_READELF_ROW = re.compile(r'^\s*\d+:\s')


def _parse_readelf(text):
    """Dynamic symbols from `readelf -Ws` / `readelf -WsC`.

    Only the `.dynsym` table is collected: `-Ws` also prints `.symtab`, whose
    local symbols (`deregister_tm_clones`, `crtstuff.c`, ...) are not exports
    and would let rule B see a class name that is never actually exported.
    Undefined entries (`Ndx UND`) are dropped for the same reason.
    """
    names = set()
    table = None
    for line in text.split('\n'):
        header = _READELF_TABLE.search(line)
        if header:
            table = header.group(1)
            continue
        if not _READELF_ROW.match(line):
            continue
        if table is not None and table != '.dynsym':
            continue
        parts = line.split()
        if len(parts) < 8:
            continue
        if parts[6] in ('UND', '*UND*'):
            continue
        name = ' '.join(parts[7:])
        if name and not name.startswith('*'):
            names.add(name)
    return names


_OBJDUMP_ROW = re.compile(
    r'^\s*[0-9a-fA-F]+\s+(?:\S+\s+)?\S+\s+(\*UND\*|\.\S+)\s+'
    r'[0-9a-fA-F]+\s+(.+?)\s*$')


def _parse_objdump(text):
    """Dynamic symbols from `objdump -T` / `objdump -TC`.

    Columns are `address [flags] type section<TAB>size name`; the section is
    `.text`/`.data`/`*UND*` and the name is the remainder of the line, so a
    demangled name with spaces (`crpc::operator new(unsigned long)`) survives.
    `*UND*` entries are imports, not exports, and are dropped.
    """
    names = set()
    for line in text.split('\n'):
        m = _OBJDUMP_ROW.match(line)
        if not m:
            continue
        section, name = m.group(1), m.group(2)
        if section == '*UND*' or not name:
            continue
        names.add(name)
    return names


def read_exports_elf(shared_object, nm='nm', readelf='readelf', objdump='objdump'):
    """Dynamic symbols of a Linux/macOS shared object.

    Both the mangled and the demangled spelling are collected, because neither
    alone is enough. The mangled form (`_ZN4crpc5http213strip_paddingEv`) needs
    an Itanium demangler the gate does not have, and rule B works on name
    segments, so a class whose only symbol is `_ZN4crpc2io9real_sinkD1Ev` would
    look unexported. The demangled form (`crpc::io::real_sink::~real_sink()`)
    splits on `::` into usable segments, so it is what actually satisfies rule B
    on Linux. Merging both means a symbol the demangler could not handle still
    contributes its raw text, and a symbol it handled contributes segments.

    `nm` is the preferred reader, but it is not always installed (a minimal
    container, a Windows checkout opened from WSL). Rather than depend on it,
    the gate walks a chain -- `nm`, then `readelf -Ws`, then `objdump -T` -- and
    uses the first reader that yields a usable table. A reader that is missing,
    that returns no symbols, or that cannot demangle is skipped, and the reason
    is recorded. Only when *every* reader is unusable does the gate raise, which
    `main` turns into exit 2 ("could not run"). A gate that cannot read the table
    must never report green, so there is no silent fallback to an empty set.

    A demangler that is absent is detected behaviourally rather than by a flag
    check: if the demangled run returns exactly the same symbols as the plain
    run *and* those symbols are Itanium-mangled (`_Z...`), nothing was
    demangled. Comparing the two runs is what makes this robust -- a reader that
    silently ignores the demangle flag (measured on `nm`: exit 0, mangled
    output, which a returncode check alone would accept) is caught, while a
    genuinely C-only library, whose symbols have no `_Z` prefix, is not.

    `nm -D` is the GNU spelling for the dynamic table; macOS spells it `-gU`
    (external, defined-only). The flag is chosen per platform and the result is
    validated rather than assumed -- a reader that silently ignores the flags
    returns an empty table, which is skipped.
    """
    readers = (
        ('nm', _nm_argv(nm, False), _nm_argv(nm, True), _parse_nm),
        ('readelf', [readelf, '-Ws'], [readelf, '-WsC'], _parse_readelf),
        ('objdump', [objdump, '-T'], [objdump, '-TC'], _parse_objdump),
    )
    errors = []
    for label, mangled_argv, demangled_argv, parse in readers:
        try:
            mangled = parse(_run_tool(mangled_argv + [shared_object]))
            demangled = parse(_run_tool(demangled_argv + [shared_object]))
        except FileNotFoundError:
            errors.append('%s: not found' % label)
            continue
        except OSError as exc:
            errors.append('%s: %s' % (label, exc))
            continue
        if not mangled and not demangled:
            errors.append('%s: no dynamic symbols parsed' % label)
            continue
        if mangled == demangled and any(s.startswith('_Z') for s in mangled):
            errors.append(
                '%s: demangler did not demangle (rule B cannot be evaluated '
                'without demangled names)' % label)
            continue
        return mangled | demangled
    raise RuntimeError(
        'no dynamic symbols parsed from %s; tried: %s'
        % (shared_object, '; '.join(errors)))


def _qualified(key):
    """`crpc::ns::Class` for a (scope-chain, name) key."""
    chain, name = key
    return '::'.join(['crpc'] + list(reversed(chain)) + [name])


def exported_classes(exported):
    """Class names mentioned as a scope segment by at least one export.

    Two mangling schemes reach this function, so it handles both.

    Itanium (Linux, `nm -D`): `crpc::io::real_sink::~real_sink()` or the raw
    `_ZN4crpc2io9real_sinkD1Ev`. Splitting on `::` after dropping the argument
    list gives `crpc`, `io`, `real_sink`; template arguments are removed first so
    `span<int>` contributes `span`. Without this branch every demangled name
    stayed one segment and rule B fired on the entire Linux library.

    MSVC (Windows, `dumpbin /exports`): `??0completion@io@crpc@@` puts the name
    straight after `??0`/`??1`, so a plain `@name@` containment test misses
    constructors and destructors. Splitting on `@` and stripping the special-name
    prefix handles both shapes, and keeps a substring match from confusing
    `span` with `obs::span`.

    The special names that put a *letter* after the `?` must be stripped too:
    `??_7` (vftable), `??_G`/`??_E` (scalar/vector deleting destructor), `??_D`
    (vbase destructor), `??_8`/`??_9` (thunks), `??_C` (string literal).
    Without this, `??_7widget@crpc@@6B@` yields the segments
    `['6B', '_7widget', 'crpc']` and the class is missed. Today every class with
    a vftable also has an ordinary exported symbol, so the gap is latent -- but
    a class whose only out-of-line entity is its vftable would be reported as
    having no export at all.

    `??_R*` (RTTI descriptors) are skipped outright rather than parsed. Their
    name position depends on the descriptor kind -- `_R0?AVname@@@8` puts the
    name after a two-character type-kind prefix, `_R1A@?0A@EA@name@@8` after an
    access-specifier run, and `_R2`/`_R3`/`_R4` put it straight after the digit
    with no prefix at all -- so a single parser cannot handle them, and audit A
    measured misses on `_R2`, `_R3` and `_R4` from trying. They never name an
    exported entity either: a type descriptor is not an API symbol, and this
    tree's DLL exports zero of them. Skipping them means a class whose *only*
    export would have been its RTTI descriptor is reported by rule B, which is
    the correct answer -- nothing callable was exported.

    Known limitation, deliberately not fixed: a *template* prefix is not
    normalised, so `?size@?$span@H@crpc@@QEBAHXZ` contributes `?$span` and not
    `span`. It cannot change a verdict today. `?$` only appears in the class-name
    position for a template, rule B skips templates (`info['template']`), and in
    the real DLL every one of the ~300 symbols containing `?$` has it inside an
    argument or template-argument list (`V?$basic_string@...`), never as the
    owning scope. Fixing it would mean re-implementing template-name parsing,
    which is exactly the kind of partial parser that produced the `??_R*` misses.
    """
    found = set()

    def add(name):
        if name:
            found.add(name)

    for sym in exported:
        # Demangled form (`crpc::obs::baggage::open()`, from `nm -C`). Splitting
        # it on `@` yields one segment, so every decorated class would look
        # unexported and rule B would fire on the whole library. Split on `::`
        # instead, and drop the argument list and any template arguments so
        # `span<int>` counts as `span`.
        head = sym.split('(', 1)[0]
        if '::' in head:
            head = re.sub(r'<[^<>]*>', '', head)
            for segment in head.split('::'):
                add(segment)
            continue
        body = sym[1:] if sym.startswith('?') else sym
        if body.startswith('?'):
            body = body[1:]
            if body.startswith('_R'):
                continue
            if body[:1] in '0123456789':
                body = body[1:]
            elif body.startswith('_'):
                # `_7name` vftable, `_G`/`_E` deleting dtor, `_D` vbase dtor,
                # `_8`/`_9` thunks, `_C` literal: underscore plus one character.
                body = body[2:]
        for segment in body.split('@'):
            add(segment)
    return found


def conditional_regions(raw_text):
    """[(first_line, last_line, platform)] for every preprocessor conditional.

    Needed because a header can declare a type that only exists on one
    platform: `epoll_backend` is wrapped in `#if defined(CRPC_OS_LINUX)`, so a
    Windows build exports nothing for it and that is correct, not a defect.
    A gate that cannot tell those apart forces a permanent allowlist entry,
    and a permanent entry is exactly what hides the next real defect.

    `#else` of a platform test is the *other* platform, not "unknown": an
    `#if defined(CRPC_OS_LINUX) ... #else ... #endif` puts its else-branch on
    Windows. Returning None there would make rule B report a false failure for
    every class in that branch. `#else` of a feature test (a `__cplusplus`
    check, a `__has_include`) really is unknown and stays None.
    """
    lines = raw_text.split('\n')
    eof = len(lines) + 1
    regions = []
    stack = []
    for number, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith('#if'):
            stack.append([number, eof, platform_of(stripped)])
        elif stripped.startswith('#else') or stripped.startswith('#elif'):
            if stack:
                start, _end, platform = stack[-1]
                stack[-1][1] = number
                if stripped.startswith('#elif'):
                    stack.append([number, eof, platform_of(stripped)])
                else:
                    stack.append([number, eof, complement_platform(platform)])
        elif stripped.startswith('#endif'):
            if stack:
                start, end, platform = stack.pop()
                regions.append((start, end, platform))
    for start, end, platform in stack:
        regions.append((start, end, platform))
    return regions


def platform_of(condition):
    """'linux' / 'windows' / None for a preprocessor condition string."""
    lowered = condition.lower()
    if 'crpc_os_linux' in lowered or '__linux__' in lowered:
        return 'linux'
    if 'crpc_os_windows' in lowered or '_win32' in lowered:
        return 'windows'
    return None


def complement_platform(platform):
    """The other platform for a negated platform test, else None."""
    if platform == 'linux':
        return 'windows'
    if platform == 'windows':
        return 'linux'
    return None


# ---------------------------------------------------------------------------
# Baseline: reviewed exceptions for implementation-detail types
# ---------------------------------------------------------------------------

def read_baseline(path):
    entries = {}
    if not os.path.exists(path):
        return entries
    for line in io.open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        key, _, reason = line.partition('\t')
        entries[key.strip()] = reason.strip()
    return entries


_BASELINE_HEADER = """\
# Reviewed exceptions for scripts/export_gate.py.
#
# Format: <key>\\t<reason>
#
# Keys:
#   class:<name>   a class in a public header with out-of-line members that is
#                  an implementation detail: it must NOT be exported, so it
#                  carries no CRPC_API. Adding a key asserts exactly that.
#
# Every entry is a decision, not a convenience. The gate fails on a stale entry
# as well as a missing one, so an entry cannot outlive the symbol it excuses.
#
# Regenerate with:
#   python scripts/export_gate.py --build-dir <dir> --update-baseline
"""


def write_baseline(path, keys, old):
    lines = [_BASELINE_HEADER.rstrip('\n')]
    for key in keys:
        lines.append('%s\t%s' % (key, old.get(key, 'implementation detail')))
    io.open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--root', default=os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    parser.add_argument('--build-dir', default=None,
                        help='shared build dir; required for rules B and D')
    parser.add_argument('--dumpbin', default=os.environ.get('CRPC_DUMPBIN', 'dumpbin'))
    parser.add_argument('--nm', default=os.environ.get('CRPC_NM', 'nm'),
                        help='ELF dynamic-symbol reader (nm)')
    parser.add_argument('--readelf', default=os.environ.get('CRPC_READELF', 'readelf'),
                        help='ELF dynamic-symbol reader fallback (readelf -Ws)')
    parser.add_argument('--objdump', default=os.environ.get('CRPC_OBJDUMP', 'objdump'),
                        help='ELF dynamic-symbol reader fallback (objdump -T)')
    parser.add_argument('--baseline', default=None)
    parser.add_argument('--update-baseline', action='store_true')
    args = parser.parse_args(argv)

    include_dir = os.path.join(args.root, 'include')
    if not os.path.isdir(include_dir):
        sys.stderr.write('export_gate: %s has no include/\n' % args.root)
        return 2

    index = scan_headers(include_dir)
    baseline_path = args.baseline or os.path.join(
        args.root, 'scripts', 'export_baseline.txt')

    exported = None
    if args.build_dir:
        # Windows: crpc.dll. Linux/macOS: libcrpc.so / libcrpc.dylib. Rules A,
        # C and D are platform independent; only rule B needs the table, and it
        # must work on the platform the gate runs on rather than silently
        # skipping there.
        candidates = ['crpc.dll', 'libcrpc.so', 'libcrpc.dylib']
        found = [os.path.join(args.build_dir, n) for n in candidates
                 if os.path.exists(os.path.join(args.build_dir, n))]
        if not found:
            sys.stderr.write('export_gate: no crpc.dll / libcrpc.so in %s\n'
                             % args.build_dir)
            return 2
        try:
            if found[0].endswith('.dll'):
                exported = read_exports(args.dumpbin, found[0])
            else:
                exported = read_exports_elf(
                    found[0], nm=args.nm, readelf=args.readelf,
                    objdump=args.objdump)
        except (OSError, RuntimeError) as exc:
            sys.stderr.write('export_gate: %s\n' % exc)
            return 2

    # Structural violations are collected separately from the baseline diff so
    # that `--update-baseline` can rewrite the file without also silencing the
    # rules that are not about the baseline. Suppressing them was a real hole:
    # `--update-baseline` used to return before rules B, C and E ran, so one
    # command could edit the file *and* swallow an undecorated free function, an
    # undecorated base or a decorated class template.
    violations = []
    baseline_diff = []

    # Rule D: a class template must not be decorated.
    for key, info in sorted(index.classes.items()):
        if info['template'] and info['api']:
            violations.append(
                ('D', '%s:%d' % (info['file'], info['line']),
                 'class template %s carries CRPC_API; a consumer that '
                 'instantiates another specialisation fails to link'
                 % _qualified(key)))

    # Rule A: a class with out-of-line members must be decorated, unless it is
    # a reviewed implementation detail.
    baseline = read_baseline(baseline_path)
    needs_export = {}
    for key, info in sorted(index.classes.items()):
        if info['template'] or info['api'] or not info['out_of_line_member']:
            continue
        needs_export['class:' + _qualified(key)] = info

    new = sorted(k for k in needs_export if k not in baseline)
    stale = sorted(k for k in baseline if k not in needs_export)

    for key in new:
        info = needs_export[key]
        baseline_diff.append(
            ('A', '%s:%d' % (info['file'], info['line']),
             'class %s has out-of-line members but no CRPC_API, and is not in '
             'the baseline' % key.split(':', 1)[1]))
    for key in stale:
        baseline_diff.append(
            ('A', os.path.basename(baseline_path),
             'stale baseline entry %s (the class no longer needs the exception)'
             % key))

    # Rule B: a decorated class must export something.
    if exported is not None:
        exported_scopes = exported_classes(exported)
        host = 'windows' if os.name == 'nt' else 'linux'
        for key, info in sorted(index.classes.items()):
            if not info['api'] or info['template']:
                continue
            # A class whose members are all defined in the header has no
            # entity that needs to cross the module boundary, so there is
            # nothing for rule B to require. The two toolchains disagree
            # about what they emit for such a class -- MSVC exports every
            # inline member of a dllexport class (`?value@publish_result@
            # crpc@@QEBA?AW4code@12@XZ`), GCC emits no dynamic symbol at all
            # because the members are never odr-used inside the library and
            # `visibility("default")` only affects entities that are
            # emitted -- and neither difference is visible to a consumer,
            # which compiles the same inline definitions from the header.
            # Without this, rule B demanded exports that ELF deliberately
            # does not produce for 20 value types (`publish_result`,
            # `directory_entry`, `node_health`, ...).
            if not info['out_of_line_member']:
                continue
            # A class inside `#if defined(CRPC_OS_LINUX)` cannot be exported by
            # a Windows build; the platform tag makes that explicit instead of
            # forcing a permanent allowlist entry.
            if info.get('platform') and info['platform'] != host:
                continue
            if info['name'] not in exported_scopes:
                violations.append(
                    ('B', '%s:%d' % (info['file'], info['line']),
                     'class %s carries CRPC_API but has no exported symbol'
                     % info['name']))

    # Rule E: an exported class must not have an undecorated base.
    #
    # MSVC rejects this itself, but only as C4275, and only when the base is
    # used as a base of a *dllexport* class -- so the failure is a warning that
    # /WX turns into an error, which means it is caught today by luck of the
    # warning policy rather than by a stated rule. Rule E makes that explicit.
    #
    # The evidence for the rule is C4275, not RTTI. Audit A measured a consumer
    # deriving from a *decorated* base (`sink`) and from an *undecorated* one
    # (`codec`) and found `SECT*` local definitions of `??0`/`??1`/`??_7`/`??_R0`
    # in **both** cases, with zero `__imp_` imports either way: CRPC_API does not
    # make a consumer import the base's vtable or type descriptor. So the harm is
    # a duplicate local copy of the base subobject's ctor/dtor/vtable per
    # translation unit, which is an ODR violation and a real C4275, and it is
    # *not* detectable by comparing `typeid` (MSVC emits a per-module descriptor
    # regardless and `type_info::operator==` falls back to a name compare).
    #
    # Rule A cannot cover these: their members are pure virtual, so
    # out_of_line_member is false by construction.
    #
    # This deliberately does not assert "an abstract class must be decorated".
    # Measured on this tree, "undecorated + non-template + pure virtual" selects
    # 8 types and adding aggregates pushes the count into the hundreds, so the
    # rule would either mis-fire on header-only value types or be unverifiable.
    # Audit A showed the eight header-only callback interfaces (`codec`,
    # `instance_set`, `multipart_handler`, `interceptor`, `exporter`,
    # `stream_transport`, `quorum`, `call_state_base`) need no macro: the library
    # receives them by reference or pointer, the consumer implements them, so the
    # consumer never needs the DLL's copy of the base -- link and run both
    # succeed with and without the macro, and so does deriving from an
    # undecorated base. Rule E flags exactly the case that matters (a decorated
    # class deriving from an undecorated *non-template* base) and reports zero on
    # a clean tree.
    #
    # A template base is skipped when the base list is parsed (`'<' not in
    # base`), and that is correct rather than a blind spot. A template base is
    # necessarily header-only: the consumer instantiates it from the header, so
    # there is no single definition to import and no cross-boundary copy to
    # disagree about. Audit A built a shared library with a decorated derived
    # class over an undecorated template base and over
    # `std::enable_shared_from_this`: build exit 0, zero warnings even at /W4,
    # no C4275 -- because the warning needs a base with an out-of-line
    # definition to disagree about. Audit B measured the same shape at the symbol
    # level: the derived class's own ctor is `UNDEF` (imported from the DLL)
    # while the template base's ctor/dtor are `SECT*` local definitions in both
    # the decorated and the undecorated case. Checking template bases would
    # demand CRPC_API on `std::enable_shared_from_this`.
    by_name = {}
    for key, info in index.classes.items():
        by_name.setdefault(info['name'], []).append((key, info))
    for key, info in sorted(index.classes.items()):
        if not info['api'] or info['template']:
            continue
        for base in info.get('bases', []):
            candidates = by_name.get(base, [])
            if not candidates:
                continue  # a base outside this tree (std::, a forward decl)
            if all(c[1]['api'] for c in candidates):
                continue
            violations.append(
                ('E', '%s:%d' % (info['file'], info['line']),
                 'exported class %s derives from undecorated base %s; the base '
                 'must carry CRPC_API so consumers use the library copy'
                 % (info['name'], base)))

    # Rule C: a namespace-scope declaration without a body must be decorated.
    for key, info in sorted(index.free_decls.items()):
        if key in index.free_defs:
            continue
        if not info['api']:
            violations.append(
                ('C', '%s:%d' % (info['file'], info['line']),
                 'free function %s is declared without a body but has no '
                 'CRPC_API' % _qualified(key)))

    print('export_gate: %d header classes, %d free declarations, %d baseline'
          % (len(index.classes), len(index.free_decls), len(baseline)))
    if exported is not None:
        print('export_gate: %d exported names' % len(exported))

    # Coverage counters, printed on every run.
    #
    # A gate that only prints OK cannot be distinguished from a gate that
    # silently stopped seeing things -- and every one of the six blindness bugs
    # found by the two audits failed in that direction: it printed OK. These
    # counts make the failure mode observable. If M1 drops sharply on a clean
    # tree, the scanner is broken, not the code.
    #
    # They measure four disjoint sets, and the names say which:
    #   M1 decorated + out-of-line members   -> what rule A would catch if the
    #                                           macro were removed (the potential
    #                                           coverage of rule A)
    #   M2 undecorated + out-of-line members -> what rule A catches today (must
    #                                           be 0 unless baselined)
    #   M3 decorated + no out-of-line member -> outside rule A entirely; these
    #                                           are interfaces and thin wrappers
    #                                           whose only exports are in src/
    #   M4 undecorated + no out-of-line      -> header-only value types and
    #                                           implementation helpers; nothing
    #                                           to export, so nothing to check
    # M1+M2+M3+M4+T equals the class count, which is what makes a silent drop in
    # any bucket visible: the total is printed on the same screen, so a reader
    # can check the partition instead of trusting three numbers that happen to
    # add up to 81 out of 267.
    m1 = m2 = m3 = m4 = t = 0
    for info in index.classes.values():
        if info['template']:
            t += 1
        elif info['out_of_line_member']:
            if info['api']:
                m1 += 1
            else:
                m2 += 1
        elif info['api']:
            m3 += 1
        else:
            m4 += 1
    print('export_gate: M1=%d (decorated, out-of-line members) '
          'M2=%d (undecorated, out-of-line members) '
          'M3=%d (decorated, header-only) '
          'M4=%d (undecorated, header-only) T=%d (templates) | other frames=%d'
          % (m1, m2, m3, m4, t, index.other_frames))
    if m1 + m2 + m3 + m4 + t != len(index.classes):
        print('export_gate: FAIL -- coverage buckets do not partition the %d '
              'indexed classes; the scanner is inconsistent'
              % len(index.classes))
        violations.append(('M', 'scripts/export_gate.py',
                           'coverage counters do not sum to the class count'))

    # The baseline is a record of reviewed exceptions, not a pass signal, so
    # writing it is allowed to succeed while the tree still has rule A
    # differences -- otherwise the file could never be bootstrapped (the first
    # run is by definition all-new). Every other rule still has to be clean on
    # that run, which is what `violations` holds.
    if args.update_baseline:
        write_baseline(baseline_path, sorted(needs_export), baseline)
        added = len(new)
        removed = len(stale)
        print('export_gate: baseline written, %d entr(ies) -> %s (+%d / -%d)'
              % (len(needs_export), baseline_path, added, removed))
        if not violations:
            print('export_gate: OK (baseline rewritten; rule A diff not judged)')
            return 0
        # Fall through to the failure report: a structural defect must be fixed
        # rather than recorded.
    else:
        violations.extend(baseline_diff)

    if violations:
        print('export_gate: FAIL -- %d violation(s)' % len(violations))
        for rule, where, message in violations:
            print('  [%s] %s: %s' % (rule, where, message))
        return 1
    print('export_gate: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
