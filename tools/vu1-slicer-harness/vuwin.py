"""Disassemble a window of a VU1 image. Lower-op decode copied from RemixVU1Slice.cpp so it
matches the backend exactly: LQ dest = ft (5-bit), base = is (4-bit), imm = signed 11-bit;
LQI = low 0x3C sub 13, LQD = low 0x3E sub 13; XGKICK = low 0x3C sub 27.
Usage: vuwin.py <bin> <from_pc_hex> <to_pc_hex>
"""
import struct, sys

BC = 'xyzw'
def ft(c): return (c >> 16) & 0x1F
def fs(c): return (c >> 11) & 0x1F
def fd(c): return (c >> 6) & 0x1F
def is_(c): return (c >> 11) & 0xF
def imm11(c): return (c & 0x3FF) - 0x400 if (c & 0x400) else (c & 0x3FF)

MAIN = {0: 'ADD', 4: 'SUB', 8: 'MADD', 12: 'MSUB', 16: 'MAX', 20: 'MINI', 24: 'MUL'}
FD = {0: 'ADDA', 1: 'SUBA', 2: 'MADDA', 3: 'MSUBA', 4: 'ITOF', 5: 'FTOI', 6: 'MULA', 7: 'MULAq/ABS/CLIP',
      8: 'ADDAq', 10: 'ADDAi', 12: 'MULAi', 11: 'NOP?', 13: 'ADDA', 15: 'MULA(vec)', 16: 'OPMULA', 17: 'MADDA(vec)'}

def upper(c):
    op = c & 0x3F
    e = ' [E]' if c & 0x40000000 else ''
    if op < 0x20 and (op & ~3) in MAIN:
        return '%-6s vf%02d <- vf%02d * vf%02d.%s%s' % (MAIN[op & ~3] + BC[op & 3], fd(c), fs(c), ft(c), BC[op & 3], e)
    if op >= 0x3C:
        sub = (c >> 6) & 0x1F
        name = FD.get(sub, 'FD%d.%d' % (op - 0x3C, sub))
        if sub in (0, 1, 2, 3, 6):
            return '%-6s ACC  <- vf%02d * vf%02d.%s%s' % (name + BC[op - 0x3C], fs(c), ft(c), BC[op - 0x3C], e)
        if sub == 11 and op == 0x3F:
            return 'NOP' + e
        return '%-6s fs=vf%02d ft=vf%02d%s' % (name, fs(c), ft(c), e)
    return 'op%02x   fd=vf%02d fs=vf%02d ft=vf%02d%s' % (op, fd(c), fs(c), ft(c), e)

def lower(c):
    op = c >> 25
    if op == 0:
        return 'LQ   vf%02d <- %d(vi%02d)' % (ft(c), imm11(c), is_(c))
    if op == 1:
        return 'SQ   vf%02d -> %d(vi%02d)' % (fs(c), imm11(c), (c >> 16) & 0xF)
    if op == 0x40:
        low, sub = c & 0x3F, (c >> 6) & 0x1F
        if low == 0x3C and sub == 13: return 'LQI  vf%02d <- (vi%02d++)' % (ft(c), is_(c))
        if low == 0x3E and sub == 13: return 'LQD  vf%02d <- (--vi%02d)' % (ft(c), is_(c))
        if low == 0x3C and sub == 27: return 'XGKICK vi%02d' % is_(c)
        if low == 0x3C and sub == 14: return 'DIV'
        return ''
    if op in (32, 33, 36, 37): return 'BRANCH(uncond)'
    if op in (40, 41, 44, 45, 46, 47): return 'BRANCH(cond)'
    return ''

def main():
    data = open(sys.argv[1], 'rb').read()[:0x4000]
    w = struct.unpack('<4096I', data)
    a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
    for pc in range(a & ~7, b + 1, 8):
        lo, up = w[pc // 4], w[pc // 4 + 1]
        print('0x%04x  %-40s | %s' % (pc, upper(up), lower(lo)))

main()
