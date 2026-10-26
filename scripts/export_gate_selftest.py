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
import os
import re
import shutil
import subprocess
import sys

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
    print('')
    if _failures:
        print('export_gate_selftest: FAIL -- %d check(s): %s'
              % (len(_failures), ', '.join(_failures)))
        return 1
    print('export_gate_selftest: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
