#!/usr/bin/env python3
import hashlib, os, shutil
from pathlib import Path

SM_AUTH_HASH_ITERATIONS = 100000
PASSWORD = b'Ap39wb6301@@'
TARGETS = [Path('data/users.csv'), Path('build/data/users.csv')]

for path in TARGETS:
    if not path.exists():
        print(f'skipping missing: {path}')
        continue
    bak = path.with_suffix(path.suffix + '.bak')
    if not bak.exists():
        shutil.copy2(path, bak)
        print(f'backup created: {bak}')
    else:
        print(f'backup exists: {bak}')

    salt = os.urandom(16)
    h = hashlib.sha256(salt + PASSWORD).digest()
    for _ in range(1, SM_AUTH_HASH_ITERATIONS):
        h = hashlib.sha256(h).digest()
    new_line = f"admin,{salt.hex()},{h.hex()},ADMIN,0,0\n"

    lines = path.read_text().splitlines()
    with path.open('w', newline='') as f:
        for line in lines:
            if line.startswith('admin,'):
                f.write(new_line)
            else:
                f.write(line + '\n')
    print(f'updated admin in: {path}')
print('done')
