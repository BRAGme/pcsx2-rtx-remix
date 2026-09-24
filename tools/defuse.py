"""What computes vf23..vf26? Dump every instruction that DEFINES them, with operands,
and follow the operands back one level to see where they come from.

Full upper-op decode this time, not just the MULA/MADDA/MADD subset, so we can see
MUL/ADD/SUB/MOVE/etc. that the slicer ignores.
"""
import struct, sys, os

VU1_PROGSIZE = 16384
BC = ['x', 'y', 'z', 'w']

def ft(c): return (c >> 16) & 0x1F
def fs(c): return (c >> 11) & 0x1F
def fd(c): return (c >> 6) & 0x1F
def it_(c): return (c >> 16) & 0xF
def is_(c): return (c >> 11) & 0xF
def id_(c): return (c >> 6) & 0xF

# microVU_Tables.inl opcode map for the UPPER pipe.
UPPER_MAIN = {
    0x00: 'ADDbc', 0x01: 'ADDbc', 0x02: 'ADDbc', 0x03: 'ADDbc',
    0x04: 'SUBbc', 0x05: 'SUBbc', 0x06: 'SUBbc', 0x07: 'SUBbc',
    0x08: 'MADDbc', 0x09: 'MADDbc', 0x0A: 'MADDbc', 0x0B: 'MADDbc',
    0x0C: 'MSUBbc', 0x0D: 'MSUBbc', 0x0E: 'MSUBbc', 0x0F: 'MSUBbc',
    0x10: 'MAXbc', 0x11: 'MAXbc', 0x12: 'MAXbc', 0x13: 'MAXbc',
    0x14: 'MINIbc', 0x15: 'MINIbc', 0x16: 'MINIbc', 0x17: 'MINIbc',
    0x18: 'MULbc', 0x19: 'MULbc', 0x1A: 'MULbc', 0x1B: 'MULbc',
    0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi',
    0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi', 0x23: 'MADDi',
    0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi',
    0x28: 'ADD',  0x29: 'MADD',  0x2A: 'MUL',  0x2B: 'MAX',
    0x2C: 'SUB',  0x2D: 'MSUB',  0x2E: 'OPMSUB', 0x2F: 'MINI',
}
# The four FD_xx sub-tables at op >= 0x3C, indexed by (code>>6)&0x1F.
FD_SUB = {
    0: 'ADDAbc', 1: 'SUBAbc', 2: 'MADDAbc', 3: 'MSUBAbc',
    4: 'ITOF', 5: 'FTOI', 6: 'MULAbc', 7: 'MULAq/ABS/CLIP',
    8: 'ADDAq', 9: 'SUBAq', 10: 'ADDAi', 11: 'SUBAi',
    12: 'MULAi', 13: 'ADDA', 14: 'SUBA', 15: 'MULA',
    16: 'OPMULA', 17: 'MADDA', 18: 'MSUBA', 19: 'NOP',
}

def disasm_upper(code):
    op = code & 0x3F
    if op in UPPER_MAIN:
        m = UPPER_MAIN[op]
        if m.endswith('bc'):
            return (m[:-2] + BC[op & 3], fd(code), fs(code), ft(code))
        return (m, fd(code), fs(code), ft(code))
    if op >= 0x3C:
        sub = (code >> 6) & 0x1F
        bc = op - 0x3C
        name = FD_SUB.get(sub, 'FD_%d.%d' % (bc, sub))
        if name.endswith('bc'):
            name = name[:-2] + BC[bc]
        # ACC-writing ops have no fd; report fd as ACC
        return (name, -1, fs(code), ft(code))
    return ('?op%02x' % op, fd(code), fs(code), ft(code))

def lower_load(code):
    op = code >> 25
    if op == 0:
        return ('LQ', it_(code), code & 0x7FF)
    if op == 0x40:
        low = code & 0x3F
        sub = (code >> 6) & 0x1F
        if low == 0x37 and sub == 0: return ('LQI', it_(code), None)
        if low == 0x37 and sub == 1: return ('LQD', it_(code), None)
    return (None, None, None)

def run(path, targets, window=None):
    data = open(path, 'rb').read()[:VU1_PROGSIZE]
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    print('=== %s ===' % os.path.basename(path))
    hits = 0
    for pc in range(0, len(data) - 8 + 1, 8):
        if window and not (window[0] <= pc <= window[1]):
            continue
        lower = words[pc // 4]
        upper = words[pc // 4 + 1]
        name, d, s, t = disasm_upper(upper)
        lk, ldest, limm = lower_load(lower)
        define_upper = (d in targets and d >= 0)
        define_lower = (lk is not None and ldest in targets)
        if define_upper or define_lower:
            hits += 1
            parts = []
            if define_upper:
                parts.append('UPPER %-9s vf%02d <- fs=vf%02d ft=vf%02d' % (name, d, s, t))
            if define_lower:
                parts.append('LOWER %-3s vf%02d <- imm=%s' % (lk, ldest, limm))
            print('  0x%04x  %s' % (pc, '   |   '.join(parts)))
    if hits == 0:
        print('  (nothing defines %s here)' % sorted(targets))
    print()

if __name__ == '__main__':
    path = sys.argv[1]
    targets = set(int(x) for x in sys.argv[2].split(','))
    win = None
    if len(sys.argv) > 3:
        a, b = sys.argv[3].split('-')
        win = (int(a, 16), int(b, 16))
    run(path, targets, win)
