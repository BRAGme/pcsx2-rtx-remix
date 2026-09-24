"""Minimal VU1 upper-op disassembler + chain tracer.

Ported from pcsx2/GS/Remix/RemixVU1Slice.cpp so the classification matches the backend's
exactly: classify_upper() at :43, field_ft/fs/fd at :19-21, classify_load() at :72.
Instruction layout is [lower_u32, upper_u32] little-endian, 8 bytes per instruction,
which is how RemixVU1Capture writes the raw 16 KB image.
"""
import struct, sys, collections

VU1_PROGSIZE = 16384

BC = ['x', 'y', 'z', 'w']

def ft(c): return (c >> 16) & 0x1F
def fs(c): return (c >> 11) & 0x1F
def fd(c): return (c >> 6) & 0x1F
def it_(c): return (c >> 16) & 0xF
def is_(c): return (c >> 11) & 0xF
def id_(c): return (c >> 6) & 0xF

def classify_upper(code):
    """-> (kind, bc). kind in MULA/MADDA/MADD/CLIP/OTHER"""
    op = code & 0x3F
    if 8 <= op <= 11:
        return ('MADD', op - 8)
    if op >= 0x3C:
        sub = (code >> 6) & 0x1F
        bc = op - 0x3C
        if sub == 6:
            return ('MULA', bc)
        if sub == 2:
            return ('MADDA', bc)
        if sub == 7 and op == 0x3F:
            return ('CLIP', bc)
    return ('OTHER', 0)

def classify_lower(code):
    """Only what the slicer cares about: LQ-family loads that fill a vf register."""
    op = code >> 25
    if op == 0:
        return ('LQ', it_(code), code & 0x7FF)      # dest vf = ft, imm
    if op == 0x40:
        low = code & 0x3F
        sub = (code >> 6) & 0x1F
        if low == 0x37 and sub == 0:
            return ('LQI', it_(code), 0)
        if low == 0x37 and sub == 1:
            return ('LQD', it_(code), 0)
    return (None, 0, 0)

def load_prog(path):
    data = open(path, 'rb').read()
    assert len(data) >= VU1_PROGSIZE, len(data)
    return struct.unpack('<%dI' % (VU1_PROGSIZE // 4), data[:VU1_PROGSIZE])

def trace(path, start_pc, verbose_chains=6):
    words = load_prog(path)
    vf_loaded = {}                       # vf reg -> pc where a load wrote it
    chains = []
    active = None
    pc = start_pc & ~7
    while pc + 8 <= VU1_PROGSIZE:
        lower = words[pc // 4]
        upper = words[pc // 4 + 1]
        kind, bc = classify_upper(upper)
        F, S, D = ft(upper), fs(upper), fd(upper)

        if kind == 'MULA':
            active = {'start': pc, 'vertex_vf': F, 'mask': 0, 'rows': {},
                      'ops': [], 'found': 0}
            chains.append(active)

        if active is not None and kind in ('MULA', 'MADDA', 'MADD'):
            accepted = (F == active['vertex_vf'] or F == 0)
            active['ops'].append((pc, kind, bc, S, F, D, accepted))
            if accepted:
                active['rows'][bc] = (S, vf_loaded.get(S))
                active['mask'] |= (1 << bc)

        if active is not None and kind == 'MADD' and active['mask'] == 0xF:
            active['complete_at'] = pc
            active['result_vf'] = D
            active['found'] = sum(1 for b in active['rows'] if active['rows'][b][1] is not None)
            active = None

        lk, ldest, limm = classify_lower(lower)
        if lk:
            vf_loaded[ldest] = (pc, lk, limm)

        pc += 8
    return chains

def summarise(path, start_pc):
    chains = trace(path, start_pc)
    complete = [c for c in chains if 'complete_at' in c]
    print('%s  start_pc=0x%04x' % (path.split(chr(92))[-1].split('/')[-1], start_pc))
    print('  chains started %d  complete %d' % (len(chains), len(complete)))

    # Why do incomplete chains stop? Report the mask they reached and the reason.
    reasons = collections.Counter()
    for c in chains:
        if 'complete_at' in c:
            continue
        mask = c['mask']
        rejected = sum(1 for o in c['ops'] if not o[6])
        reasons[(bin(mask), 'ft-rejected-ops=%d' % rejected)] += 1
    print('  INCOMPLETE chains, by (mask reached, ops rejected on the ft filter):')
    for k, v in reasons.most_common(8):
        print('     mask=%-8s %-22s  x%d' % (k[0], k[1], v))

    # What terminal op did the chains that DID reach mask 0xF use?
    for c in complete[:3]:
        print('  COMPLETE chain @0x%04x -> vf%02d  found=%d/4 rows-from-memory' %
              (c['start'], c['result_vf'], c['found']))
        for (opc, kind, bc, S, F, D, acc) in c['ops']:
            src = c['rows'].get(bc)
            note = ''
            if src and src[1]:
                note = '   row%s <- %s @0x%04x imm=%d' % (BC[bc], src[1][1], src[1][0], src[1][2])
            elif kind != 'OTHER':
                note = '   row%s <- vf%02d (REGISTER-RESIDENT, never loaded here)' % (BC[bc], S)
            print('     0x%04x %-6s%s fs=vf%02d ft=vf%02d fd=vf%02d%s%s' %
                  (opc, kind, BC[bc], S, F, D, '' if acc else ' [ft-REJECTED]', note))
    print()

if __name__ == '__main__':
    for arg in sys.argv[1:]:
        p, _, spc = arg.rpartition(',')
        summarise(p, int(spc, 16))
