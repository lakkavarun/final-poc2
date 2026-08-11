import hashlib
import os
import shutil
from pathlib import Path

path = Path('data/users.csv')
backup = Path('data/users.csv.bak')
if not path.exists():
    raise SystemExit('data/users.csv not found')
if not backup.exists():
    shutil.copy2(path, backup)
    print(f'Backup created: {backup}')
else:
    print(f'Backup already exists: {backup}')

password = b'Ap39wb6301@@'
salt = os.urandom(16)
h = hashlib.sha256(salt + password).digest()
for _ in range(1, 100000):
    h = hashlib.sha256(h).digest()

new_line = f'admin,{salt.hex()},{h.hex()},ADMIN,0,0\n'
lines = path.read_text().splitlines()
with path.open('w', newline='') as f:
    for line in lines:
        if line.startswith('admin,'):
            f.write(new_line)
        else:
            f.write(line + '\n')

print('Admin password reset to default: Ap39wb6301@@')
