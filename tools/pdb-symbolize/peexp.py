"""Attribute profile samples in a system DLL using its PE EXPORT table.

No PDB is needed, but the naming is NEAREST PRECEDING EXPORT, not a real range
match -- an internal helper is reported under whatever export sits below it. Good
enough to tell heap from wait from lock; not good enough to quote as exact.
"""
import struct, sys, bisect, collections


def exports(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    dd = pe + 24 + (112 if magic == 0x20B else 96)          # DataDirectory
    exp_rva, exp_size = struct.unpack_from('<II', d, dd)
    if exp_rva == 0:
        return []

    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + i * 40
        # VirtualSize(+8), VirtualAddress(+12), SizeOfRawData(+16), PointerToRawData(+20).
        vsz, va, rawsz, raw = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, max(vsz, rawsz), raw))

    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return raw + (rva - va)
        return None

    e = off(exp_rva)
    nfun, nname = struct.unpack_from('<II', d, e + 20)
    afun, aname, aord = struct.unpack_from('<III', d, e + 28)
    fo, no, oo = off(afun), off(aname), off(aord)
    out = []
    for i in range(nname):
        nrva = struct.unpack_from('<I', d, no + 4 * i)[0]
        p = off(nrva)
        z = d.index(b'\0', p)
        name = d[p:z].decode('ascii', 'replace')
        ordinal = struct.unpack_from('<H', d, oo + 2 * i)[0]
        frva = struct.unpack_from('<I', d, fo + 4 * ordinal)[0]
        # Skip forwarders, which point back into the export directory.
        if exp_rva <= frva < exp_rva + exp_size:
            continue
        out.append((frva, name))
    out.sort()
    return out


def main():
    dll, prof, module = sys.argv[1], sys.argv[2], sys.argv[3]
    exp = exports(dll)
    starts = [e[0] for e in exp]
    sys.stderr.write('%s: %d exports\n' % (module, len(exp)))

    agg = collections.Counter()
    total = mod_total = 0
    for line in open(prof, encoding='utf-8'):
        if line.startswith('#'):
            continue
        p = line.split()
        if len(p) != 3:
            continue
        total += int(p[2])
        if p[0].lower() != module.lower():
            continue
        rva, count = int(p[1], 16), int(p[2])
        mod_total += count
        j = bisect.bisect_right(starts, rva) - 1
        agg['%s  (+%x)' % (exp[j][1], rva - exp[j][0]) if j >= 0 else '?'] += count

    print('%s = %.1f%% of all samples' % (module, 100.0 * mod_total / total))
    print('%-8s %-8s  %s' % ('samples', 'of all', 'nearest preceding export'))
    for name, c in agg.most_common(18):
        print('%-8d %6.2f%%  %s' % (c, 100.0 * c / total, name))


main()
