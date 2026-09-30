"""Compile and embed the D13x TSX example using the shared PocketJS compiler."""
import os
import shutil
import subprocess
import sys
import portenv as pe

def main():
    repo = pe.repo_root()
    local = repo / '.pocket-build/tooling/node_modules/@oven/bun-windows-x64/bin/bun.exe'
    bun = os.environ.get('BUN') or shutil.which('bun') or (str(local) if local.is_file() else None)
    if not bun:
        raise SystemExit('Bun required: set BUN to its executable and run bun install --frozen-lockfile --ignore-scripts in PocketJS')
    subprocess.run([bun, 'hosts/rt-thread/tools/build-app.ts'], cwd=repo, check=True)
    dest = repo / '.pocket-build/d13x/counter'
    subprocess.run([sys.executable, str(repo/'hosts/rt-thread/components/pocketjs_package/tools/embed_package.py'),
        '--package', str(dest/'counter.pocket'), '--host-profile',
        str(repo/'hosts/rt-thread/examples/counter/pocket.host.json'), '--name', 'counter',
        '--output-dir', str(dest/'embedded')], cwd=repo, check=True)
    return 0

if __name__ == '__main__':
    sys.exit(main())
