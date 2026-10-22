#!/usr/bin/env python3
"""Inspect a self-contained Qualcomm ZAP MBN; never uploads or authenticates it.

The peridot default carveout is the live DT gpu_microcode_region. MDT layout
follows MiCode/peridot-u-oss drivers/soc/qcom/mdt_loader.c. Firmware files and
generated metadata must remain outside git. A valid plan is not authentication.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def plan(data, base=0x9899A000, capacity=0x2000):
    if not (0 <= base < 2**64 and 0 < capacity <= 2**64 - base) or base & 4095 or capacity & 4095:
        raise ValueError('invalid physical carveout')
    if not 52 <= len(data) <= 4 * 1024 * 1024:
        raise ValueError('ELF length must be52B..4MiB')
    e = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if e[0][:7] != b'\x7fELF\x01\x01\x01' or e[1] != 2 or e[3] != 1:
        raise ValueError('expected ELF32 little-endian executable')
    phoff, ehsize, phsize, count = e[5], e[8], e[9], e[10]
    if ehsize != 52 or phsize != 32 or not 2 <= count <= 64 or phoff < 52:
        raise ValueError('unsupported program header layout')
    if phoff + count * phsize > len(data):
        raise ValueError('short program header table')
    headers = [struct.unpack_from('<8I', data, phoff + i * phsize) for i in range(count)]
    for p in headers:
        if p[1] + p[4] > len(data):
            raise ValueError('split/truncated firmware; full segments required')
    first = headers[0]
    if first[0] == 1 or first[1] != 0 or first[4] < phoff + count * phsize:
        raise ValueError('invalid MDT header segment')
    hashes = [p for p in headers[1:] if p[0] != 1 and p[6] & 0x07000000 == 0x02000000]
    if len(hashes) != 1 or not hashes[0][4]:
        raise ValueError('expected exactly one hash/signature segment')
    loads = [p for p in headers if p[0] == 1 and p[5] and p[6] & 0x07000000 != 0x02000000]
    if not loads:
        raise ValueError('no loadable segment')
    for p in loads:
        if p[4] > p[5] or p[3] + p[5] > 2**32:
            raise ValueError('invalid load size/address')
    low = min(p[3] for p in loads)
    high = max((p[3] + p[5] + 4095) & ~4095 for p in loads)
    relocatable = any(p[6] & (1 << 27) for p in loads)
    if relocatable and high - low > capacity:
        raise ValueError('aligned relocation span exceeds carveout')
    origin = low if relocatable else base
    segments = []
    for p in loads:
        offset = p[3] - origin
        if offset < 0 or offset + p[5] > capacity:
            raise ValueError('load exceeds reserved GPU carveout')
        if any(offset < s['offset_in_carveout'] + s['memory_bytes'] and
               s['offset_in_carveout'] < offset + p[5] for s in segments):
            raise ValueError('overlapping load segments')
        segments.append(dict(file_offset=p[1], file_bytes=p[4], memory_bytes=p[5],
                             offset_in_carveout=offset, physical_address=hex(base + offset)))
    h = hashes[0]
    metadata = data[:first[4]] + data[h[1]:h[1] + h[4]]
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                pas_id=13, carveout_base=hex(base), carveout_bytes=capacity,
                relocatable=relocatable, pas_mem_setup_bytes=high-low if relocatable else None,
                metadata_bytes=len(metadata), metadata_sha256=hashlib.sha256(metadata).hexdigest(),
                metadata_parts=[dict(offset=0, bytes=first[4]), dict(offset=h[1], bytes=h[4])],
                segments=segments, authenticated=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('image', type=Path)
    ap.add_argument('--base', type=lambda s: int(s, 0), default=0x9899A000)
    ap.add_argument('--capacity', type=lambda s: int(s, 0), default=0x2000)
    a = ap.parse_args()
    try:
        result = plan(a.image.read_bytes(), a.base, a.capacity)
    except (OSError, ValueError, struct.error) as exc:
        ap.exit(1, f'ZAP image refused: {exc}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
