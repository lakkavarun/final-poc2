import hashlib
ITER=100000
pwd=b'Ap39wb6301@@'
with open('build/data/users.csv','r') as f:
    for l in f:
        if l.startswith('admin,'):
            parts=l.strip().split(',')
            salt_hex=parts[1]
            stored=parts[2]
            break
salt=bytes.fromhex(salt_hex)
h=hashlib.sha256(salt+pwd).digest()
for i in range(1,ITER):
    h=hashlib.sha256(h).digest()
print('salt=',salt_hex)
print('stored=',stored)
print('computed=',h.hex())
print('equal=',h.hex()==stored)
