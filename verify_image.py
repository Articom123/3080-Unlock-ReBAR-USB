"""Inspect the built EFI image; rejects imports and incorrect target/entry."""
import hashlib
import json
from pathlib import Path
import struct
import sys

p = Path(sys.argv[1])
d = p.read_bytes()
def u16(o): return struct.unpack_from('<H', d, o)[0]
def u32(o): return struct.unpack_from('<I', d, o)[0]
assert d[:2] == b'MZ'
pe = u32(0x3c)
assert d[pe:pe + 4] == b'PE\0\0' and u16(pe + 4) == 0x8664
op = pe + 24
assert u16(op) == 0x20b and u16(op + 68) == 10
assert struct.unpack_from('<Q', d, op + 24)[0] == 0
assert u16(op + 70) & 0x140 == 0x140
assert u32(op + 108) >= 16
for idx in (1, 12, 13):
    assert d[op + 112 + idx * 8:op + 120 + idx * 8] == bytes(8), 'unexpected imports'
assert u32(op + 116 + 5 * 8), 'relocations required'
entry = u32(op + 16)
sections = op + u16(pe + 20)
valid_entry = False
for i in range(u16(pe + 6)):
    s = sections + i * 40
    flags = u32(s + 36)
    assert flags & 0xa0000000 != 0xa0000000, 'writable executable section'
    if u32(s + 12) <= entry < u32(s + 12) + u32(s + 8):
        valid_entry = bool(flags & 0x20000000)
assert valid_entry
report = dict(file=p.name, bytes=len(d), sha256=hashlib.sha256(d).hexdigest(),
              architecture='x64', subsystem='EFI_APPLICATION', entry_rva=entry,
              imports=False, relocatable=True, hardware_tested=False)
Path(sys.argv[2]).write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
