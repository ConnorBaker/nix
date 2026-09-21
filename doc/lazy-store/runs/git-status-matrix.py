#!/usr/bin/env python3
import argparse, itertools, json, os, pathlib, shutil, subprocess, time
parser = argparse.ArgumentParser(description='Compare every declared Git status case against two libgit2 libraries')
parser.add_argument('--before-lib', required=True)
parser.add_argument('--after-lib', required=True)
parser.add_argument('--status-dump', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
args = parser.parse_args()
B = args.output.resolve()
B.mkdir(parents=True, exist_ok=True)
R = B / 'repo'
R.mkdir()
E = dict({k: v for k, v in os.environ.items() if not k.startswith('GIT_')}, GIT_AUTHOR_NAME='Test', GIT_AUTHOR_EMAIL='test@example.com', GIT_COMMITTER_NAME='Test', GIT_COMMITTER_EMAIL='test@example.com', GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1')

def git(*args, data=None):
    return subprocess.check_output(['git', '-C', str(R), *args], input=data, env=E).decode().strip()
git('init', '-q')
git('config', 'core.filemode', 'true')
git('config', 'core.autocrlf', 'false')
blob = git('hash-object', '-w', '--stdin', data=b'original\n')
link = git('hash-object', '-w', '--stdin', data=b'target')
empty = git('mktree', data=b'')
child = git('mktree', data=f'100644 blob {blob}\tchild\n'.encode())
base = git('commit-tree', empty, data=b'base\n')
moduleCommit = git('commit-tree', child, data=b'module\n')
kinds = {'absent': None, 'regular': ('100644', 'blob', blob), 'executable': ('100755', 'blob', blob), 'symlink': ('120000', 'blob', link), 'directory': ('040000', 'tree', child), 'gitlink': ('160000', 'commit', moduleCommit)}
trees = {k: git('mktree', data=f'{v[0]} {v[1]} {v[2]}\tnode\n'.encode() if v else b'') for k, v in kinds.items()}
commits = {k: git('commit-tree', v, data=(k + '\n').encode()) for k, v in trees.items()}
changed = git('hash-object', '-w', '--stdin', data=b'changed\n')
changedLink = git('hash-object', '-w', '--stdin', data=b'changed-target')
changedCommit = git('commit-tree', child, data=b'changed-commit\n')
changedChild = git('mktree', data=f'100644 blob {changed}\tchild\n'.encode())
changedObjects = {
    'directory': changedChild,
    'regular': changed,
    'executable': changed,
    'symlink': changedLink,
    'gitlink': changedCommit,
}
changedTrees = {
    k: git('mktree', data=(
        f"{v[0]} {v[1]} {changedObjects[k]}\tnode\n".encode() if v else b''
    ))
    for k, v in kinds.items()
}

# A worktree gitlink must be an actual nested repository, not another
# ordinary directory. Keep its metadata outside the worktree so each case
# can replace node freely, and use the already constructed commit objects.
module = B / 'module'
module.mkdir()
subprocess.run(['git', '-C', str(module), 'init', '-q'], env=E, check=True)
(module / '.git/objects/info/alternates').write_text(str(R / '.git/objects') + '\n')
subprocess.run(['git', '-C', str(module), 'config', 'core.worktree', str(R / 'node')], env=E, check=True)

libs = {'before': args.before_lib, 'after': args.after_lib}
records = []
start = time.monotonic()
for head, index, work, ignore, attrs, icase, indexChanged, workContent in itertools.product(kinds, kinds, kinds, ['none', 'all', 'negated'], [False, True], [False, True], [False, True], ['same', 'changed', 'eol']):
    git('update-ref', 'HEAD', commits[head])
    git('read-tree', (changedTrees if indexChanged else trees)[index])
    git('config', 'core.ignorecase', str(icase).lower())
    p = R / 'node'
    if p.is_symlink() or p.is_file():
        p.unlink()
    elif p.exists():
        shutil.rmtree(p)
    if work in ('regular', 'executable'):
        p.write_bytes({'same': b'original\n', 'changed': b'changed\n', 'eol': b'original\r\n'}[workContent])
        p.chmod(493 if work == 'executable' else 420)
    elif work == 'symlink':
        p.symlink_to('target' if workContent == 'same' else 'changed-target')
    elif work in ('directory', 'gitlink'):
        p.mkdir()
        if work == 'gitlink':
            (p / '.git').write_text('gitdir: ' + str(module / '.git') + '\n')
            git('-C', str(p), 'reset', '--hard', moduleCommit if workContent == 'same' else changedCommit)
        (p / 'child').write_bytes({'same': b'original\n', 'changed': b'changed\n', 'eol': b'original\r\n'}[workContent])
    (R / '.gitignore').write_text({'none': '', 'all': '*\n', 'negated': '*\n!node/\nnode/*\n!node/child\n'}[ignore])
    (R / '.gitattributes').write_text('* text=auto eol=lf\n' if attrs else '')
    outs = {}
    for label, lib in libs.items():
        result = subprocess.run([str(args.status_dump.resolve()), str(R)], env=dict(E, LD_LIBRARY_PATH=lib), capture_output=True)
        outs[label] = (result.returncode, result.stdout.decode(), result.stderr.decode())
    case = dict(head=head, index=index, worktree=work, ignore=ignore, attributes=attrs, ignorecase=icase, index_changed=indexChanged, work_content=workContent)
    if outs['before'] != outs['after'] or outs['after'][0] != 0:
        (B / 'mismatch.json').write_text(json.dumps(dict(case=case, outputs=outs), indent=2))
        raise RuntimeError(case)
    records.append(dict(case=case, status=outs['after'][1], returncode=outs['after'][0]))
    if len(records) % 216 == 0:
        print(f'{len(records)} cases equal, {time.monotonic() - start:.1f}s', flush=True)
(B / 'status-matrix.json').write_text(json.dumps(dict(cases=len(records), elapsed_s=time.monotonic() - start, all_equal=True, records=records), indent=2))
print('All', len(records), 'cases equal', flush=True)
