"""Self-test for scripts/export_gate.py's export-table parsers.

Run:  python scripts/export_gate_selftest.py

Three halves:

  * real -- run this machine's `dumpbin /exports` on the shared build's crpc.dll
    and prove the parser recovers the full symbol table (>1000 names, no ` = `
    suffix leaking into a name). This half *skips* when the DLL is not built or
    when `dumpbin` is not on PATH: a missing dumper is not a failure of the
    parser, and crashing here would hide the synthetic half's results.
  * synthetic dumpbin -- feed `read_exports` hand-built dumpbin output that
    exercises the shapes the real dumper emits: an ILT suffix
    (` = @ILT+1234(name)`), an annotation suffix (` = name (comment)`), and a
    long name wrapped across two lines. These cannot be produced from the real
    DLL here (it emits no wrapped rows), so they are the only way to pin the
    wrapping behaviour.
  * synthetic ELF -- exercise `read_exports_elf`'s reader chain (`nm`, then
    `readelf -Ws`, then `objdump -T`) with fake tools, so the fallback logic is
    pinned without needing binutils on the machine. A reader that is absent, that
    returns an empty table, or that cannot demangle must be skipped rather than
    crash, and a table no reader can parse must raise.

Exit code 0 = every assertion held; 1 = a failure, printed with its input.
"""
import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_gate  # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
_DLL = os.path.join(
    _ROOT, '.opencode', 'tmp', 'out-redis-shared-final', 'build',
    'x64-Debug-shared', 'crpc.dll')

_failures = []


def check(label, condition, detail=''):
    if condition:
        print('  ok   %s' % label)
    else:
        print('  FAIL %s %s' % (label, detail))
        _failures.append(label)


class _Proc(object):
    def __init__(self, stdout):
        self.stdout = stdout


def fake_run(payload):
    def run(_cmd, **_kw):
        return _Proc(payload.encode('utf-8'))
    return run


def parse(payload):
    """`read_exports` with subprocess.run replaced by `payload`."""
    original = subprocess.run
    subprocess.run = fake_run(payload)
    try:
        return export_gate.read_exports('dumpbin', 'fake.dll')
    finally:
        subprocess.run = original


def _dispatch(rules):
    """A subprocess.run replacement that picks a payload per command.

    `rules` is a list of `(predicate, result)` pairs; the first predicate that
    matches `cmd` wins. `result` is a string payload or an Exception to raise.
    A command no predicate matches raises `FileNotFoundError`, so a test can make
    a reader absent without touching the real PATH.
    """
    def run(cmd, **_kw):
        for predicate, result in rules:
            if predicate(cmd):
                if isinstance(result, Exception):
                    raise result
                return _Proc(result.encode('utf-8'))
        raise FileNotFoundError(cmd[0])
    return run


def with_run(rules, fn):
    """Run `fn` with subprocess.run replaced by `rules`, then restore it."""
    original = subprocess.run
    subprocess.run = _dispatch(rules)
    try:
        return fn()
    finally:
        subprocess.run = original


# ---------------------------------------------------------------------------
# Real DLL
# ---------------------------------------------------------------------------
def test_real_dll():
    print('real: %s' % _DLL)
    if not os.path.exists(_DLL):
        print('  skip (dll not built)')
        return
    dumpbin = os.environ.get('CRPC_DUMPBIN', 'dumpbin')
    if shutil.which(dumpbin) is None:
        print('  skip (dumpbin not on PATH; CRPC_DUMPBIN=%r)' % dumpbin)
        return
    names = export_gate.read_exports(dumpbin, _DLL)
    check('parses > 1000 names', len(names) > 1000, 'got %d' % len(names))
    check('no ` = ` suffix leaked into a name',
          not any(' = ' in n for n in names))
    check('no comment leaked into a name',
          not any('(' in n or ')' in n for n in names))
    check('unmangled C symbol present', 'crpc_version_string' in names)
    check('mangled C++ symbol present',
          any(n.startswith('??0') or n.startswith('?') for n in names))
    print('  (%d names parsed)' % len(names))


# ---------------------------------------------------------------------------
# Synthetic dumpbin parser shapes
# ---------------------------------------------------------------------------
def test_suffixes():
    print('synthetic: ILT and annotation suffixes')
    payload = (
        'Microsoft (R) COFF/PE Dumper\n'
        'Dump of file fake.dll\n'
        '\n'
        '    ordinal hint RVA      name\n'
        '\n'
        '          1    0 00001000 foo = @ILT+256(foo)\n'
        '          2    1 00002000 bar = bar (int `void bar(void)\')\n'
        '          3    2 00003000 ??0cls@ns@crpc@@QEAA@XZ\n'
        '\n'
        '  Summary\n'
        '\n'
        '        1000 .text\n'
    )
    names = parse(payload)
    check('ILT suffix stripped', 'foo' in names and 'foo = @ILT+256(foo)' not in names, sorted(names))
    check('annotation suffix stripped', 'bar' in names, sorted(names))
    check('plain name kept', '??0cls@ns@crpc@@QEAA@XZ' in names, sorted(names))
    check('section rows not absorbed', '.text' not in names and '1000' not in names, sorted(names))


def test_wrapped_name():
    print('synthetic: name wrapped across two lines')
    payload = (
        'Dump of file fake.dll\n'
        '    ordinal hint RVA      name\n'
        '\n'
        '          1    0 00001000 ??0a_very_long_class_name@ns@crpc@@QEAA@\n'
        '          XZ = @ILT+256(??0a_very_long_class_name@ns@crpc@@QEAA@XZ)\n'
        '          2    1 00002000 short = @ILT+300(short)\n'
        '\n'
        '  Summary\n'
    )
    names = parse(payload)
    check('wrapped name rejoined without a space',
          '??0a_very_long_class_name@ns@crpc@@QEAA@XZ' in names, sorted(names))
    check('wrapped name suffix dropped',
          not any(n.startswith('??0a_very_long') and ' = ' in n for n in names))
    check('entry after the wrap still parsed', 'short' in names, sorted(names))


def test_no_exports_raises():
    print('synthetic: genuinely empty table still raises')
    payload = (
        'Dump of file fake.dll\n'
        '    ordinal hint RVA      name\n'
        '\n'
        '  Summary\n'
    )
    try:
        parse(payload)
    except RuntimeError as exc:
        check('raises RuntimeError', 'no exports parsed' in str(exc), str(exc))
    else:
        check('raises RuntimeError', False, 'no exception raised')


# ---------------------------------------------------------------------------
# Synthetic ELF reader chain
# ---------------------------------------------------------------------------
_NM_MANGLED = (
    '000000000000112e T _ZN4crpc5http210plain_freeEi\n'
    '000000000000113e T crpc_c_api\n'
)
_NM_DEMANGLED = (
    '000000000000112e T crpc::http2::plain_free(int)\n'
    '000000000000113e T crpc_c_api\n'
)
_READLEF_MANGLED = (
    "Symbol table '.dynsym' contains 8 entries:\n"
    '   Num:    Value          Size Type    Bind   Vis      Ndx Name\n'
    '     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND \n'
    '     1: 0000000000000000     0 NOTYPE  WEAK   DEFAULT  UND __cxa_finalize\n'
    '     5: 000000000000112e    16 FUNC    GLOBAL DEFAULT   10 _ZN4crpc5http210plain_freeEi\n'
    '     8: 000000000000113e    19 FUNC    GLOBAL DEFAULT   10 crpc_c_api\n'
    '    12: 0000000000004008     8 OBJECT  GLOBAL DEFAULT   20 _ZN4crpc19crpc_version_stringE\n'
    '\n'
    "Symbol table '.symtab' contains 32 entries:\n"
    '   Num:    Value          Size Type    Bind   Vis      Ndx Name\n'
    '     2: 0000000000001040     0 FUNC    LOCAL  DEFAULT   10 deregister_tm_clones\n'
)
_READLEF_DEMANGLED = (
    "Symbol table '.dynsym' contains 8 entries:\n"
    '   Num:    Value          Size Type    Bind   Vis      Ndx Name\n'
    '     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND \n'
    '     1: 0000000000000000     0 NOTYPE  WEAK   DEFAULT  UND __cxa_finalize\n'
    '     5: 000000000000112e    16 FUNC    GLOBAL DEFAULT   10 crpc::http2::plain_free(int)\n'
    '     8: 000000000000113e    19 FUNC    GLOBAL DEFAULT   10 crpc_c_api\n'
    '    12: 0000000000004008     8 OBJECT  GLOBAL DEFAULT   20 crpc::crpc_version_string\n'
    '\n'
    "Symbol table '.symtab' contains 32 entries:\n"
    '   Num:    Value          Size Type    Bind   Vis      Ndx Name\n'
    '     2: 0000000000001040     0 FUNC    LOCAL  DEFAULT   10 deregister_tm_clones\n'
)
_OBJDUMP_MANGLED = (
    '\n'
    'fake.so:     file format elf64-x86-64\n'
    '\n'
    'DYNAMIC SYMBOL TABLE:\n'
    '0000000000000000  w   D  *UND*\t0000000000000000 __cxa_finalize\n'
    '000000000000112e g    DF .text\t0000000000000010 _ZN4crpc5http210plain_freeEi\n'
    '000000000000113e g    DF .text\t0000000000000013 crpc_c_api\n'
)
_OBJDUMP_DEMANGLED = (
    '\n'
    'fake.so:     file format elf64-x86-64\n'
    '\n'
    'DYNAMIC SYMBOL TABLE:\n'
    '0000000000000000  w   D  *UND*\t0000000000000000 __cxa_finalize\n'
    '000000000000112e g    DF .text\t0000000000000010 crpc::http2::plain_free(int)\n'
    '000000000000113e g    DF .text\t0000000000000013 crpc_c_api\n'
)


def test_nm_primary():
    print('synthetic: nm is the preferred reader')
    names = with_run([
        (lambda c: c[0] == 'nm' and '-C' in c, _NM_DEMANGLED),
        (lambda c: c[0] == 'nm', _NM_MANGLED),
    ], lambda: export_gate.read_exports_elf('fake.so'))
    check('demangled name kept', 'crpc::http2::plain_free(int)' in names, sorted(names))
    check('mangled name kept', '_ZN4crpc5http210plain_freeEi' in names, sorted(names))


def test_nm_missing_readelf_fallback():
    print('synthetic: nm missing falls back to readelf')
    names = with_run([
        (lambda c: c[0] == 'nm', FileNotFoundError('nm')),
        (lambda c: c[0] == 'readelf' and '-WsC' in c, _READLEF_DEMANGLED),
        (lambda c: c[0] == 'readelf', _READLEF_MANGLED),
    ], lambda: export_gate.read_exports_elf('fake.so'))
    check('readelf demangled name kept',
          'crpc::http2::plain_free(int)' in names, sorted(names))
    check('readelf symtab local dropped',
          'deregister_tm_clones' not in names, sorted(names))


def test_readelf_missing_objdump_fallback():
    print('synthetic: nm and readelf missing fall back to objdump')
    names = with_run([
        (lambda c: c[0] == 'nm', FileNotFoundError('nm')),
        (lambda c: c[0] == 'readelf', FileNotFoundError('readelf')),
        (lambda c: c[0] == 'objdump' and '-TC' in c, _OBJDUMP_DEMANGLED),
        (lambda c: c[0] == 'objdump', _OBJDUMP_MANGLED),
    ], lambda: export_gate.read_exports_elf('fake.so'))
    check('objdump demangled name kept',
          'crpc::http2::plain_free(int)' in names, sorted(names))
    check('objdump undefined import dropped',
          '__cxa_finalize' not in names, sorted(names))


def test_nm_no_demangle_falls_back():
    print('synthetic: nm present but not demangling falls back to readelf')
    names = with_run([
        (lambda c: c[0] == 'nm', _NM_MANGLED),
        (lambda c: c[0] == 'readelf' and '-WsC' in c, _READLEF_DEMANGLED),
        (lambda c: c[0] == 'readelf', _READLEF_MANGLED),
    ], lambda: export_gate.read_exports_elf('fake.so'))
    check('demangled name from fallback kept',
          'crpc::http2::plain_free(int)' in names, sorted(names))
    check('result is readelf mangled|demangled, not nm mangled-only',
          names == (export_gate._parse_readelf(_READLEF_MANGLED)
                    | export_gate._parse_readelf(_READLEF_DEMANGLED)),
          sorted(names))


def test_all_readers_missing_raises():
    print('synthetic: every reader missing raises a clear error')
    try:
        with_run([
            (lambda c: c[0] == 'nm', FileNotFoundError('nm')),
            (lambda c: c[0] == 'readelf', FileNotFoundError('readelf')),
            (lambda c: c[0] == 'objdump', FileNotFoundError('objdump')),
        ], lambda: export_gate.read_exports_elf('fake.so'))
    except RuntimeError as exc:
        text = str(exc)
        check('raises RuntimeError', 'no dynamic symbols parsed' in text, text)
        check('names every missing reader',
              all(t in text for t in ('nm', 'readelf', 'objdump')), text)
    else:
        check('raises RuntimeError', False, 'no exception raised')


def test_empty_table_raises():
    print('synthetic: a present-but-empty table raises')
    try:
        with_run([
            (lambda c: c[0] in ('nm', 'readelf', 'objdump'), ''),
        ], lambda: export_gate.read_exports_elf('fake.so'))
    except RuntimeError as exc:
        check('raises RuntimeError', 'no dynamic symbols parsed' in str(exc), str(exc))
    else:
        check('raises RuntimeError', False, 'no exception raised')


def test_readelf_parser_filters():
    print('synthetic: readelf parser drops .symtab locals and UND imports')
    names = export_gate._parse_readelf(_READLEF_MANGLED)
    check('dynsym symbol kept', '_ZN4crpc5http210plain_freeEi' in names, sorted(names))
    check('symtab local dropped', 'deregister_tm_clones' not in names, sorted(names))
    check('undefined import dropped', '__cxa_finalize' not in names, sorted(names))
    check('empty UND name dropped', '' not in names, sorted(names))


def test_objdump_parser_filters():
    print('synthetic: objdump parser drops UND imports')
    names = export_gate._parse_objdump(_OBJDUMP_MANGLED)
    check('defined symbol kept', '_ZN4crpc5http210plain_freeEi' in names, sorted(names))
    check('undefined import dropped', '__cxa_finalize' not in names, sorted(names))


def test_nm_parser_keeps_spaces():
    print('synthetic: nm parser keeps a demangled name with spaces')
    names = export_gate._parse_nm(_NM_DEMANGLED)
    check('space-bearing name kept whole',
          'crpc::http2::plain_free(int)' in names, sorted(names))



# ---------------------------------------------------------------------------
# Header scanner: declaration classification
#
# These pin the two rules a plain `python scripts/export_gate.py` run only
# exercises through the tree's current contents. Without them, dropping the
# initialiser guard or the inline-definition exemption from `function_name` /
# `_scan` would keep printing OK on this tree until the offending headers
# changed, and the gate would quietly go back to false positives.
# ---------------------------------------------------------------------------
def _scan(src):
    index = export_gate.HeaderIndex()
    text = export_gate.strip_directives(
        export_gate.strip_comments_and_literals(src))
    export_gate._scan(text, 'test.hpp', index)
    return index


def test_declaration_shapes():
    print('synthetic: declaration classification')
    fn = export_gate.function_name

    check('member with an initialiser is not a declaration',
          fn('std::uint32_t capabilities = mask(1) | mask(2)') is None)
    check('constexpr variable ending in () is not a function',
          fn('constexpr double grid_inf = std::numeric_limits<double>::infinity()')
          is None)
    check('constexpr variable with a cast is not a function',
          fn('constexpr std::size_t grid_none = static_cast<std::size_t>(-1)')
          is None)
    check('a default argument is still a declaration',
          fn('std::size_t f(std::size_t x = 1)') == 'f')
    check('pure virtual is skipped', fn('virtual void read() = 0') is None)
    check('deleted is skipped', fn('connection(const connection&) = delete') is None)
    check('the defaulted ctor is skipped', fn('socket() = default') is None)
    check('operator= keeps its name',
          fn('options& operator=(const options& other)') == 'operator=')
    check('real member is a declaration', fn('void close() noexcept') == 'close')
    # A `const` return type must not be mistaken for a `constexpr` variable; a
    # blanket `const ` prefix in _SKIP_KEYWORDS hid every const-qualified
    # member function and made rule A miss real out-of-line members.
    check('const member function is a declaration',
          fn('const std::string& name() const') == 'name')
    check('const-qualified getter is a declaration',
          fn('const char* data() const noexcept') == 'data')


def test_inline_definition_exemption():
    print('synthetic: inline definition exemption')
    idx = _scan('''
namespace crpc { namespace detail {
[[nodiscard]] client* live_owner(const call_core& core) noexcept;
inline client* live_owner(const call_core& core) noexcept { return nullptr; }
} }
''')
    check('declaration recorded',
          any(n == 'live_owner' for (_chain, n) in idx.free_decls))
    check('header definition recorded',
          any(n == 'live_owner' for (_chain, n) in idx.free_defs))

    idx2 = _scan('''
namespace crpc { namespace detail {
client* never_defined(const call_core& core) noexcept;
} }
''')
    check('body-less declaration recorded',
          any(n == 'never_defined' for (_chain, n) in idx2.free_decls))
    check('body-less declaration is not a definition',
          not any(n == 'never_defined' for (_chain, n) in idx2.free_defs))


def test_initialiser_member_is_not_out_of_line():
    print('synthetic: initialiser member is not out-of-line')
    idx = _scan('''
namespace crpc {
struct options {
    std::uint32_t capabilities = mask(1) | mask(2);
    std::size_t rows = 100;
};
class socket {
public:
    void close() noexcept;
};
}
''')
    opt = [v for k, v in idx.classes.items() if k[1] == 'options']
    check('options indexed', len(opt) == 1, opt)
    check('options has no out-of-line member',
          bool(opt) and not opt[0]['out_of_line_member'])
    sock = [v for k, v in idx.classes.items() if k[1] == 'socket']
    check('a real member is still detected',
          bool(sock) and sock[0]['out_of_line_member'])


def test_exported_classes_shapes():
    print('synthetic: exported_classes name extraction')
    scopes = export_gate.exported_classes([
        'crpc::io::real_sink::~real_sink()',
        'crpc::obs::span<int>::size() const',
        '??0completion@io@crpc@@QEAAXXZ',
        'crpc::io::real_sink::~real_sink(int, char*)',
    ])
    check('Itanium member scope', 'real_sink' in scopes)
    check('template arguments stripped', 'span' in scopes)
    check('MSVC ctor prefix stripped', 'completion' in scopes)
    check('a name is not welded to its punctuation',
          not any(',' in s or '(' in s or ')' in s for s in scopes if s),
          sorted(scopes))

    # A class that only ever appears as a parameter type is not "exported": the
    # rule B check must not treat it as such, which is why the scan stops at the
    # argument list.
    scopes2 = export_gate.exported_classes(['crpc::take(crpc::widget)'])
    check('parameter type alone is not an export', 'widget' not in scopes2,
          sorted(scopes2))

# ---------------------------------------------------------------------------
# Rule F: over-export / ship-view seam difference
#
# These pin the checker in BOTH directions. A vacuous checker (one that always
# returns []) would still pass `ship clean -> OK`; the seam-present FAIL, the
# x64/x86 manglings and the three read_exports shapes make sure a broken
# detection is caught. The end-to-end tests exercise main()'s activation gates
# (--build-dir table, --tests-enabled / CMakeCache view, --update-baseline,
# exit 2) so a regression cannot hide behind a lower-level helper that still
# works in isolation.
# ---------------------------------------------------------------------------
_F_SEAM_X64 = '?set_stale_retries@registry_client@cluster@crpc@@QEAAXH@Z'
_F_SEAM_X86 = '?set_stale_retries@registry_client@cluster@crpc@@QAEXH@Z'
_F_SEAM_REQID = ('?set_request_identity@registry_client@cluster@crpc@@'
                 'QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@'
                 'V?$allocator@D@2@@std@@@Z')
_F_PROD = '?check_acl@registry_service@cluster@crpc@@QEBAXXZ'
_F_PLAIN = 'crpc_version_string'


def _f_headers():
    return (
        'Dump of file fake.dll\n'
        '    ordinal hint RVA      name\n'
        '\n'
    )


def _f_summary():
    return '\n  Summary\n\n        1000 .text\n'


def test_rule_f_ship_seam_fails():
    print('rule F: a ship DLL exporting a test-only seam MUST fail')
    v = export_gate.rule_f_check({_F_SEAM_X64, _F_PLAIN}, False)
    check('seam exported in ship -> 1 violation',
          len(v) == 1 and v[0][0] == 'F', v)
    check('violation names the seam',
          bool(v) and 'set_stale_retries' in v[0][2], v)
    check('evidence is attached (verifiable, not comment-derived)',
          bool(v) and 'P-C9.out.txt' in v[0][2], v)
    v2 = export_gate.rule_f_check({_F_SEAM_REQID}, False)
    check('4th seam set_request_identity also caught',
          len(v2) == 1 and 'set_request_identity' in v2[0][2], v2)


def test_rule_f_ship_clean_ok():
    print('rule F: a ship DLL with no test-only seams MUST pass')
    v = export_gate.rule_f_check({_F_PLAIN, _F_PROD}, False)
    check('clean ship table -> no violation', v == [], v)
    # Production dual-use symbols must never be flagged (they are the explicit
    # whitelist; guarding them breaks -Examples -NoTests with LNK2019).
    for name, _ev in export_gate._PROD_DUAL_USE:
        check('whitelist %s is not a seam' % name,
              name not in dict(export_gate._TEST_ONLY_SEAMS))


def test_rule_f_view_gate():
    print('rule F: view gating (test-enabled / unknown / no table)')
    seam = {_F_SEAM_X64}
    check('test-enabled view (True) skips',
          export_gate.rule_f_check(seam, True) == [])
    check('unknown view (None) skips',
          export_gate.rule_f_check(seam, None) == [])
    check('no table (None exported) skips',
          export_gate.rule_f_check(None, False) == [])
    check('ship view (False) is the only active one',
          len(export_gate.rule_f_check(seam, False)) == 1)


def test_rule_f_mangling_and_symbols():
    print('rule F: x64/x86 mangling + symbol classification')
    check('x64 QEAA mangling parsed',
          export_gate.seam_method(_F_SEAM_X64) == 'set_stale_retries',
          export_gate.seam_method(_F_SEAM_X64))
    check('x86 QAE (stdcall-ish) mangling parsed',
          export_gate.seam_method(_F_SEAM_X86) == 'set_stale_retries',
          export_gate.seam_method(_F_SEAM_X86))
    check('ELF demangled member parsed',
          export_gate.seam_method('crpc::cluster::registry_client::'
                                  'set_token_issued_at(long)')
          == 'set_token_issued_at')
    check('constructor special name is not a plain member',
          export_gate.seam_method('??0registry_client@cluster@crpc@@QEAA@XZ')
          is None)
    check('RTTI special name is not a plain member',
          export_gate.seam_method('??_7registry_client@cluster@crpc@@6B@')
          is None)
    check('plain C symbol has no seam',
          export_gate.seam_method(_F_PLAIN) is None)
    check('buckets are disjoint (fail-closed self-consistency)',
          not (set(dict(export_gate._TEST_ONLY_SEAMS))
               & {n for n, _e in export_gate._PROD_DUAL_USE}))
    v = export_gate.rule_f_check({_F_SEAM_X86, _F_SEAM_X64}, False)
    check('x86 seam FAILs in ship, x64 deduped (one violation per seam)',
          len(v) == 1, v)


def test_rule_f_3_shapes_via_read_exports():
    print('rule F: ILT / annotation / wrapped shapes through read_exports')
    # (1) ILT suffix, (2) annotation suffix, (3) a name wrapped across two
    # lines. All three are fed through the REAL parser (no regex rewrite), so
    # seam detection is proven against the shapes the field code documented at
    # export_gate.py:67-80.
    payload = (_f_headers()
               + '          1    0 00001000 ' + _F_SEAM_X64
               + ' = @ILT+141195(' + _F_SEAM_X64 + ')\n'
               + '          2    1 00002000 ' + _F_SEAM_X86
               + ' = ' + _F_SEAM_X86 + ' (void `public: void ...\')\n'
               + '          3    2 00003000 '
               '?set_request_identity@registry_client@cluster@crpc@@QEAAXA\n'
               + '          EBV?$basic_string@DU?$char_traits@D@std@@V?$'
                 'allocator@D@2@@std@@@Z = @ILT+90045(?set_request_identity@'
                 'registry_client@cluster@crpc@@QEAAXAEBV?$basic_string@DU?$'
                 'char_traits@D@std@@V?$allocator@D@2@@std@@@Z)\n'
               + '          4    3 00004000 ' + _F_PLAIN + '\n'
               + _f_summary())
    names = parse(payload)
    check('ILT-suffix seam parsed', _F_SEAM_X64 in names, sorted(names))
    check('annotation-suffix seam parsed', _F_SEAM_X86 in names, sorted(names))
    check('wrapped set_request_identity rejoined', _F_SEAM_REQID in names,
          sorted(names))
    v = export_gate.rule_f_check(names, False)
    got = sorted(x[1] for x in v)
    check('all 3 shapes -> ship FAIL (2 distinct seams: x64/x86 dedup + 4th)',
          len(v) == 2, (v, got))


def test_rule_f_anti_vacuity():
    print('rule F: anti-vacuity (monkeypatched broken checker MUST be caught)')
    seam = {_F_SEAM_X64}
    original = export_gate.seam_method
    export_gate.seam_method = lambda _sym: None  # constant-False detector
    try:
        check('broken detector -> selftest-level RED',
              export_gate.rule_f_check(seam, False) == [])
    finally:
        export_gate.seam_method = original
    check('restored detector -> GREEN again',
          len(export_gate.rule_f_check(seam, False)) == 1)


def _f_tree(root, cache_tests=None):
    os.makedirs(os.path.join(root, 'include', 'crpc'), exist_ok=True)
    io.open(os.path.join(root, 'include', 'crpc', 'probe.hpp'), 'w',
            encoding='utf-8').write(
        '#ifndef P_HPP\n#define P_HPP\nnamespace crpc {\nnamespace cluster {\n'
        '[[nodiscard]] CRPC_API int decode_allocation(int in);\n}\n}\n#endif\n')
    os.makedirs(os.path.join(root, 'scripts'), exist_ok=True)
    # main() only checks existence before read_exports, whose subprocess.run
    # is monkeypatched by run_main below -- no native dumper is launched.
    io.open(os.path.join(root, 'crpc.dll'), 'wb').write(b'MZ')
    if cache_tests is not None:
        io.open(os.path.join(root, 'CMakeCache.txt'), 'w',
                encoding='utf-8').write(
            'CRPC_BUILD_TESTS:BOOL=%s\n' % cache_tests)


def test_rule_f_main_end_to_end():
    print('rule F: main() activation gates (end-to-end)')
    # main() reads the table via read_exports(args.dumpbin or find_dumpbin()).
    # Point --dumpbin at a python wrapper that prints a synthetic table; patch
    # subprocess.run inside read_exports so no native exe runs in the selftest.
    def dispatch(payload):
        def run(cmd, **_kw):
            return _Proc(payload.encode('utf-8'))
        return run

    def run_main(root, extra, payload):
        original = subprocess.run
        subprocess.run = dispatch(payload)
        try:
            with contextlib.redirect_stdout(io.StringIO()) as buf:
                rc = export_gate.main(
                    ['--root', root, '--build-dir', root,
                     '--dumpbin', 'dumpbin'] + extra)
            return rc, buf.getvalue()
        finally:
            subprocess.run = original

    # Put the temp tree inside F:\crpc, never %TEMP% (AGENTS.md). Prefer the
    # repo's evidence dir when this file is the real scripts/ copy; a sandbox
    # copy under .opencode/sandbox/exec-shared/ has no such layout, so fall
    # back to its own directory. Both are inside the repo and both are removed
    # in the finally block below.
    preferred = os.path.join(_ROOT, '.opencode', 'tmp', 'exec-shared')
    tmp_root = preferred if os.path.isdir(preferred) else _HERE
    os.makedirs(tmp_root, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='crpc_rF_', dir=tmp_root)
    try:
        payload_ship = (_f_headers()
                        + '          1    0 00001000 ' + _F_SEAM_X64 + '\n'
                        + '          2    1 00002000 ' + _F_PLAIN + '\n'
                        + _f_summary())
        payload_clean = (_f_headers()
                         + '          1    0 00001000 ' + _F_PLAIN + '\n'
                         + '          2    1 00002000 ' + _F_PROD + '\n'
                         + _f_summary())
        _f_tree(tmp, cache_tests='ON')
        # (1) ship seam + explicit off -> FAIL (rule fires end-to-end)
        rc, out = run_main(tmp, ['--tests-enabled', 'off'], payload_ship)
        check('ship seam + --tests-enabled off -> rc=1 [F]',
              rc == 1 and '[F]' in out, (rc, out))
        # (2) ship clean + explicit off -> OK (not "everything non-API fails")
        rc, out = run_main(tmp, ['--tests-enabled', 'off'], payload_clean)
        check('ship clean table -> rc=0 OK (4879 legal symbols tolerated)',
              rc == 0 and 'OK' in out and '[F]' not in out, (rc, out))
        # (3) test-enabled view (CMakeCache ON) -> seam legal -> OK
        rc, out = run_main(tmp, [], payload_ship)
        check('CMakeCache CRPC_BUILD_TESTS=ON -> seam legal, rc=0',
              rc == 0 and 'test-enabled' in out, (rc, out))
        # (4) CMakeCache OFF (dynamic channel) -> seam FAIL without a flag
        io.open(os.path.join(tmp, 'CMakeCache.txt'), 'w',
                encoding='utf-8').write('CRPC_BUILD_TESTS:BOOL=OFF\n')
        rc, out = run_main(tmp, [], payload_ship)
        check('CMakeCache=OFF drives rule F (no explicit flag) -> [F]',
              rc == 1 and '[F]' in out, (rc, out))
        # (5) explicit --tests-enabled on overrides cache OFF -> OK
        rc, out = run_main(tmp, ['--tests-enabled', 'on'], payload_ship)
        check('explicit on overrides cache OFF -> rc=0',
              rc == 0 and '[F]' not in out, (rc, out))
        # (6) unknown view (no cache, no flag) -> skip, not a false FAIL
        os.remove(os.path.join(tmp, 'CMakeCache.txt'))
        rc, out = run_main(tmp, [], payload_ship)
        check('unknown view -> rule F skipped (rc=0, no [F])',
              rc == 0 and 'unknown' in out, (rc, out))
        # (7) --update-baseline must NOT swallow rule F
        rc, out = run_main(tmp, ['--tests-enabled', 'off',
                                 '--update-baseline'], payload_ship)
        check('--update-baseline does not swallow rule F -> [F]',
              rc == 1 and '[F]' in out, (rc, out))
        # (8) exit 2 (could-not-run) is never swallowed into OK: a malformed
        # table (no header) makes read_exports raise -> main returns 2.
        rc, out = run_main(tmp, ['--tests-enabled', 'off'],
                           'Dump of file fake.dll\n  Summary\n')
        check('malformed table -> exit 2 (not OK)', rc == 2, (rc, out))
        # (9) no --build-dir -> rule F inactive (static skip)
        original = subprocess.run
        try:
            with contextlib.redirect_stdout(io.StringIO()) as buf:
                rc = export_gate.main(['--root', tmp,
                                       '--tests-enabled', 'off'])
            out = buf.getvalue()
        finally:
            subprocess.run = original
        check('no --build-dir -> rule F inactive, rc=0',
              rc == 0 and '[F]' not in out, (rc, out))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _msvc_install_present():
    """True when an MSVC toolset directory exists on this machine.

    Deliberately independent of `export_gate`: the point of the check it
    guards is that `export_gate` can find dumpbin, so asking `export_gate`
    whether a toolchain exists would make a broken resolver silence its own
    test (which is exactly what the previous version did).
    """
    if os.name != 'nt':
        return False
    for env in (os.environ.get('ProgramFiles(x86)', ''),
                os.environ.get('ProgramFiles', '')):
        if not env:
            continue
        root = os.path.join(env, 'Microsoft Visual Studio')
        if not os.path.isdir(root):
            continue
        # `<root>\<year>\<edition>\VC\Tools\MSVC\<version>`, and BuildTools
        # installs sometimes omit the edition level, so try both depths.
        for year in os.listdir(root):
            if year == 'Installer':
                continue
            year_dir = os.path.join(root, year)
            if not os.path.isdir(year_dir):
                continue
            bases = [year_dir]
            for sub in os.listdir(year_dir):
                sub_dir = os.path.join(year_dir, sub)
                if os.path.isdir(sub_dir):
                    bases.append(sub_dir)
            for base in bases:
                if os.path.isdir(os.path.join(base, 'VC', 'Tools', 'MSVC')):
                    return True
    return False


def test_find_dumpbin():
    print('synthetic: dumpbin resolution')
    # An explicit path always wins, so --dumpbin and CRPC_DUMPBIN keep working.
    check('explicit path is returned unchanged',
          export_gate.find_dumpbin(r'C:\\tools\\dumpbin.exe')
          == r'C:\\tools\\dumpbin.exe')

    # On a machine that has a toolchain, resolving with no argument must produce
    # something that exists -- this is the case that failed in CI: the gate was
    # invoked from a shell where dumpbin was installed but not on PATH, and it
    # died with WinError 2 instead of finding the binary.
    if os.name == 'nt':
        resolved = export_gate.find_dumpbin()
        on_path = shutil.which('dumpbin')
        if on_path is None:
            # The interesting case, and the one CI hit: dumpbin is installed but
            # not on PATH, so resolution must produce a real path.
            #
            # Two earlier versions of this check were vacuous: one accepted
            # `resolved == 'dumpbin'`, and one asked
            # `find_dumpbin_has_toolchain()` -- so a mutant that broke the
            # resolver also silenced the check via the skip branch. The
            # decision must come from something the code under test cannot
            # influence: scan the same install root independently.
            toolchain_present = _msvc_install_present()
            if toolchain_present:
                check('resolves to a real file when not on PATH',
                      resolved != 'dumpbin' and os.path.exists(resolved),
                      'resolved %r' % (resolved,))
                check('resolved path is dumpbin.exe',
                      resolved.lower().endswith('dumpbin.exe'), resolved)
            else:
                print('  skip (no MSVC toolchain installed on this machine)')
        else:
            check('uses the PATH entry when one exists', resolved == on_path,
                  '%r vs %r' % (resolved, on_path))
    else:
        check('non-Windows returns the bare name',
              export_gate.find_dumpbin() == 'dumpbin')


def main():
    test_real_dll()
    test_suffixes()
    test_wrapped_name()
    test_no_exports_raises()
    test_nm_primary()
    test_nm_missing_readelf_fallback()
    test_readelf_missing_objdump_fallback()
    test_nm_no_demangle_falls_back()
    test_all_readers_missing_raises()
    test_empty_table_raises()
    test_readelf_parser_filters()
    test_objdump_parser_filters()
    test_nm_parser_keeps_spaces()
    test_find_dumpbin()
    test_exported_classes_shapes()
    test_declaration_shapes()
    test_inline_definition_exemption()
    test_initialiser_member_is_not_out_of_line()
    test_rule_f_ship_seam_fails()
    test_rule_f_ship_clean_ok()
    test_rule_f_view_gate()
    test_rule_f_mangling_and_symbols()
    test_rule_f_3_shapes_via_read_exports()
    test_rule_f_anti_vacuity()
    test_rule_f_main_end_to_end()
    print('')
    if _failures:
        print('export_gate_selftest: FAIL -- %d check(s): %s'
              % (len(_failures), ', '.join(_failures)))
        return 1
    print('export_gate_selftest: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
