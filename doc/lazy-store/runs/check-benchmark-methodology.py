#!/usr/bin/env python3
"""Synthetic controls for the benchmark's independent equivalence checks.

Run: python3 doc/lazy-store/runs/check-benchmark-methodology.py
All stores and Git fixtures are temporary; no Nix evaluation or build runs.
"""
import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
from types import SimpleNamespace

here = Path(__file__).resolve().parent
def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, here / relative)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

checker = module('graphs', 'compare-store-graphs.py')
bench = module('bench', 'nixpkgs-evaluation.py')

def address(c, name):
    return '/nix/store/' + c * 32 + '-' + name

with tempfile.TemporaryDirectory(prefix='nix-methodology-') as directory:
    root = Path(directory)
    base_drvs = [address(c, 'pkg.drv') for c in 'ab']
    branch_drvs = [address(c, 'pkg.drv') for c in 'cd']
    base_outputs = [address(c, 'pkg') for c in 'fg']
    branch_outputs = [address(c, 'pkg') for c in 'hj']
    def store(where, drvs, outputs, literal=None):
        (where / 'nix/store').mkdir(parents=True)
        db = where / 'nix/var/nix/db/db.sqlite'
        db.parent.mkdir(parents=True)
        with sqlite3.connect(db) as con:
            con.executescript('create table ValidPaths(id integer,path text,ca text,deriver text);'
                              'create table Refs(referrer integer,reference integer);'
                              'create table DerivationOutputs(drv integer,id text,path text);')
            for i, (drv, output) in enumerate(zip(drvs, outputs)):
                con.execute('insert into ValidPaths values(?,?,?,NULL)', (i, drv, 'ca'))
                con.execute('insert into DerivationOutputs values(?,?,?)', (i, 'out', output))
                (where / 'nix/store' / Path(drv).name).write_text(
                    f'Derive([("out","{output}","","")],[],[],"system","builder",["case-{i}","{literal(output) if literal else ""}"],[])')
    store(root / 'base', base_drvs, base_outputs)
    store(root / 'branch', branch_drvs, branch_outputs)
    mapping = checker.compare(root / 'base', root / 'branch')['mapping']
    print('positive graph mapping: PASS')

    changed = root / 'branch/nix/store' / Path(branch_drvs[0]).name
    original = changed.read_text()
    for label, mutate in [
        ('ordinary-content', lambda text: text.replace('case-0', 'case-wrong')),
        ('trailing-newline', lambda text: text + '\n'),
        ('trailing-comma', lambda text: text.replace('["case-0",""]', '["case-0","",]')),
        ('missing-comma', lambda text: text.replace('["case-0",""]', '["case-0"""]')),
        ('input-tuple', lambda text: text.replace('],[],[],', '],(),[],', 1)),
    ]:
        modified = mutate(original)
        assert modified != original
        changed.write_text(modified)
        try:
            checker.compare(root / 'base', root / 'branch')
        except AssertionError:
            print(label + ' derivation mutation: REJECTED')
        else:
            raise AssertionError(label + ' derivation mutation accepted')
        changed.write_text(original)

    # Address renaming reverses both input lists' physical order. These are
    # the only two ATerm lists whose order the checker may normalize.
    for label, drvs, outputs, consumer, output in [
        ('order-base', base_drvs, base_outputs, address('k', 'consumer.drv'), address('m', 'consumer')),
        ('order-branch', branch_drvs[::-1], branch_outputs[::-1], address('l', 'consumer.drv'), address('n', 'consumer')),
    ]:
        where = root / label
        store(where, drvs, outputs)
        with sqlite3.connect(where / 'nix/var/nix/db/db.sqlite') as con:
            con.execute('insert into ValidPaths values(?,?,?,NULL)', (2, consumer, 'ca'))
            con.execute('insert into DerivationOutputs values(?,?,?)', (2, 'out', output))
            con.executemany('insert into Refs values(?,?)', [(2, 0), (2, 1)])
        input_drvs = ','.join(f'("{drv}",["out"])' for drv in sorted(drvs))
        input_srcs = ','.join(f'"{drv}"' for drv in sorted(drvs))
        (where / 'nix/store' / Path(consumer).name).write_text(
            f'Derive([("out","{output}","","")],[{input_drvs}],[{input_srcs}],"system","builder",[],[])')
    reversed_mapping = checker.compare(root / 'order-base', root / 'order-branch')['mapping']
    assert [reversed_mapping[drv] for drv in base_drvs] == branch_drvs[::-1]
    print('input-list order reversal after address mapping: PASS')

    for label, literal, allowed in [
        ('different-basename', lambda p: p.rsplit('-', 1)[0] + '-unrelated', False),
        ('longer-basename', lambda p: p + '-extra', False),
        ('unmapped-address', lambda p: p[:11] + '0' + p[12:], False),
        ('different-store-prefix', lambda p: '/other' + p, True),
        ('ascii-prefix', lambda p: 'prefix' + p, True),
        ('attached-compiler-option', lambda p: '-L' + p + '/lib', True),
        ('subpath', lambda p: p + '/subdirectory/file', True),
        ('assignment', lambda p: 'key=' + p, True),
    ]:
        before, after = root / (label + '-base'), root / (label + '-branch')
        store(before, base_drvs, base_outputs, literal)
        store(after, branch_drvs, branch_outputs, literal)
        try:
            checker.compare(before, after)
        except AssertionError as error:
            assert not allowed and 'object contents differ' in str(error), (label, str(error))
            print(label + ' rewrite: REJECTED')
        else:
            assert allowed, label + ' unexpectedly rewritten'
            print(label + ' rewrite: PASS')

    # Exhaust the byte contexts independently of compiler/shell syntax.
    # Left context never changes substitution; a valid name character on
    # the right extends the basename and therefore denotes another literal.
    before, after = base_outputs[0].encode(), branch_outputs[0].encode()
    rewrite = checker.address_rewriter({base_outputs[0]: branch_outputs[0]})
    name_bytes = set(b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-._?=')
    for byte in range(256):
        context = bytes([byte])
        assert rewrite(context + before) == context + after, byte
        expected = before if byte in name_bytes else after
        assert rewrite(before + context) == expected + context, byte
    assert rewrite(b'prefix' + before) != b'changed-prefix' + after
    print('all 512 left/right byte contexts and changed surrounding literal: PASS')

    store(root / 'collapsed', branch_drvs, [branch_outputs[0]] * 2)
    try:
        checker.compare(root / 'base', root / 'collapsed')
    except AssertionError as error:
        assert 'not injective' in str(error), str(error)
        print('collapsed declared outputs: REJECTED')
    else:
        raise AssertionError('collapsed outputs accepted')

    changed = root / 'branch/nix/store' / Path(branch_drvs[0]).name
    changed.chmod(0o755)
    try:
        checker.compare(root / 'base', root / 'branch')
    except AssertionError:
        print('executable derivation mode mutation: REJECTED')
    else:
        raise AssertionError('derivation mode mutation accepted')
    changed.chmod(0o644)

    logs = root / 'logs'
    logs.mkdir()
    rows = []
    for label, drvs in [('base', base_drvs), ('branch', branch_drvs)]:
        rows.append(dict(scheme='git', label=label, id=label, phase='warm', attributes=2, trial=0, returncode=0))
        (logs / f'{label}.json').write_text(json.dumps([
            dict(attr=f'attribute-{i}', name=f'name-{i}', version='1', drvPath=drv) for i, drv in enumerate(drvs)]))
    (root / 'samples.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in rows))
    assert checker.compare_outputs(mapping, root, 'git')['samples'] == 2
    print('positive full output mapping: PASS')

    # A graph's mapping only verifies results produced with that store.
    # Swapping two same-name derivations in another store must not be
    # excused by applying the first store's correct mapping to its results.
    warm_roots = {label: root / ('warm-' + label) for label in ('base', 'branch')}
    cold_roots = {label: root / ('cold-' + label) for label in ('base', 'branch')}
    separate = []
    for row in rows:
        label = row['label']
        separate.append(dict(row, command=['nix', '--store', f'local?root={warm_roots[label]}']))
        cold = dict(row, id='cold-' + label, phase='cold',
                    command=['nix', '--store', f'local?root={cold_roots[label]}'])
        separate.append(cold)
        (logs / f"{cold['id']}.json").write_bytes((logs / f"{row['id']}.json").read_bytes())
    (root / 'samples.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in separate))
    assert checker.compare_outputs(mapping, root, 'git', roots=warm_roots)['samples'] == 2
    swapped_mapping = dict(zip(base_drvs, branch_drvs[::-1]))
    try:
        checker.compare_outputs(swapped_mapping, root, 'git', roots=cold_roots)
    except AssertionError as error:
        assert 'output differs' in str(error), str(error)
        print('different-store derivation permutation: REJECTED')
    else:
        raise AssertionError('another store mapping accepted for cold outputs')
    (root / 'samples.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in rows))

    branch = json.loads((logs / 'branch.json').read_text())
    branch[0]['drvPath'], branch[1]['drvPath'] = branch[1]['drvPath'], branch[0]['drvPath']
    (logs / 'branch.json').write_text(json.dumps(branch))
    try:
        checker.compare_outputs(mapping, root, 'git')
    except AssertionError as error:
        assert 'output differs' in str(error), str(error)
        print('swapped attribute derivations: REJECTED')
    else:
        raise AssertionError('swapped results accepted')

    git = root / 'repo'
    git.mkdir()
    def command(*args):
        return subprocess.check_output(['git', '-C', str(git), *args], stderr=subprocess.DEVNULL)
    command('init', '--object-format=sha1')
    (git / 'file').write_text('unchanged bytes')
    command('add', 'file')
    command('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
            '-c', 'core.hooksPath=/dev/null', 'commit', '-m', 'fixture')
    plain = root / 'plain'
    shutil.copytree(git, plain, ignore=shutil.ignore_patterns('.git'))
    bench.args = SimpleNamespace(git_source=git, path_source=plain)
    bench.REV = command('rev-parse', 'HEAD').decode().strip()
    assert bench.source_provenance()['index_matches_tree']
    print('positive raw tree/index/worktree provenance: PASS')
    command('update-index', '--force-remove', 'file')
    try:
        bench.source_provenance()
    except AssertionError as error:
        assert 'index differs' in str(error), str(error)
        print('staged deletion with unchanged filesystem: REJECTED')
    else:
        raise AssertionError('staged deletion accepted')
