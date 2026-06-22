#!/usr/bin/env python3
# Generate a pure scanner/hash workload: literal-name push+pop, no dict defines.
# 1M name tokens over 20K unique names = 1M hash calls at scan time.
# Output is written next to this script as bench_scan.trx.
import os, random
random.seed(42)

def rand_name(n):
    return ''.join(random.choice('abcdefghijklmnopqrstuvwxyz') for _ in range(n))

names = []
seen = set()
while len(names) < 20000:
    n = rand_name(random.randint(3, 12))
    if n not in seen:
        seen.add(n)
        names.append(n)

TOKENS_PER_LINE = 20
TOTAL_TOKENS = 1_000_000
lines = ['% pure scanner/hash benchmark']
tokens = []
for _ in range(TOTAL_TOKENS):
    tokens.append('/' + random.choice(names) + ' pop')
# pack into lines
for i in range(0, len(tokens), TOKENS_PER_LINE):
    lines.append(' '.join(tokens[i:i+TOKENS_PER_LINE]))

out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bench_scan.trx')
with open(out_path, 'w') as f:
    f.write('\n'.join(lines) + '\n')
print(f'wrote {len(tokens)} tokens, {len(names)} unique -> {out_path}')
