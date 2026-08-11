#!/usr/bin/env python3
import os,hashlib,base64,shutil
ITER=100000
pwd_bytes=os.urandom(12)
pwd=base64.urlsafe_b64encode(pwd_bytes).decode('ascii').rstrip('=') + '!A1'
# write password to file
with open('scripts/new_admin_password.txt','w') as pf:
    pf.write(pwd + '\n')
for path in ('data/users.csv','build/data/users.csv'):
    if not os.path.exists(path):
        print('skipping', path)
        continue
    bak=path + '.bak'
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
    salt=os.urandom(16)
    h=hashlib.sha256(salt+pwd.encode('utf-8')).digest()
    for i in range(1,ITER):
        h=hashlib.sha256(h).digest()
    salt_hex=salt.hex()
    hash_hex=h.hex()
    # replace admin line
    lines=open(path,'r').read().splitlines()
    out=[]
    for line in lines:
        if line.startswith('admin,'):
            out.append(f"admin,{salt_hex},{hash_hex},ADMIN,0,0")
        else:
            out.append(line)
    open(path,'w',newline='').write('\n'.join(out)+"\n")
print('done')
