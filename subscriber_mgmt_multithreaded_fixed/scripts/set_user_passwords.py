#!/usr/bin/env python3
import os
import hashlib

def compute_hash(password, salt_bytes=None):
    if salt_bytes is None:
        salt_bytes = os.urandom(16)
    ITER = 100000
    h = hashlib.sha256(salt_bytes + password.encode('utf-8')).digest()
    for _ in range(1, ITER):
        h = hashlib.sha256(h).digest()
    return salt_bytes.hex(), h.hex()

paths = [
    r"c:\Users\91733\Downloads\subscriber_mgmt_multithreaded_fixed (1)\subscriber_mgmt_multithreaded_fixed\data\users.csv",
    r"c:\Users\91733\Downloads\subscriber_mgmt_multithreaded_fixed (1)\subscriber_mgmt_multithreaded_fixed\build\data\users.csv"
]

for path in paths:
    if not os.path.exists(path):
        print(f"File not found: {path}")
        continue
    with open(path, 'r') as f:
        lines = f.read().splitlines()
    out = []
    for line in lines:
        if line.startswith('operator,'):
            s_hex, h_hex = compute_hash('12345678')
            out.append(f"operator,{s_hex},{h_hex},OPERATOR,0,0")
        elif line.startswith('viewer,'):
            s_hex, h_hex = compute_hash('12345678')
            out.append(f"viewer,{s_hex},{h_hex},VIEWER,0,0")
        else:
            out.append(line)
    with open(path, 'w', newline='') as f:
        f.write('\n'.join(out) + '\n')
    print(f"Updated passwords for operator and viewer in {path}")
