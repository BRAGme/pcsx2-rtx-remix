"""RVA -> function name by parsing the PDB directly.

System dbghelp cannot load these PDBs (SymFromAddr fails with 126), so the MSF/DBI
streams are walked by hand. Offsets that are easy to get wrong are called out; they
match reference_pdb_symbolize_without_dbghelp.
"""
import struct, sys, bisect


class MSF:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        self.block = struct.unpack_from('<I', self.d, 0x20)[0]
        dir_bytes = struct.unpack_from('<I', self.d, 0x2C)[0]
        # block MAP at 0x34, not 0x38 -- 0x38 gives a stream count in the billions.
        map_block = struct.unpack_from('<I', self.d, 0x34)[0]
        nblocks = (dir_bytes + self.block - 1) // self.block
        blocks = struct.unpack_from('<%dI' % nblocks, self.d, map_block * self.block)
        directory = b''.join(self._blk(b) for b in blocks)[:dir_bytes]

        n = struct.unpack_from('<I', directory, 0)[0]
        sizes = struct.unpack_from('<%dI' % n, directory, 4)
        p = 4 + 4 * n
        self.streams = []
        for size in sizes:
            cnt = 0 if size in (0, 0xFFFFFFFF) else (size + self.block - 1) // self.block
            idx = struct.unpack_from('<%dI' % cnt, directory, p)
            p += 4 * cnt
            self.streams.append((size if size != 0xFFFFFFFF else 0, idx))

    def _blk(self, i):
        return self.d[i * self.block:(i + 1) * self.block]

    def stream(self, i):
        size, idx = self.streams[i]
        return b''.join(self._blk(b) for b in idx)[:size]


def functions(pdb_path):
    msf = MSF(pdb_path)
    dbi = msf.stream(3)
    h = struct.unpack_from('<iIIHHHHHHiiiiiIiiHHI', dbi, 0)
    modinfo_size, seccontrib, secmap, srcinfo, tsmap = h[9], h[10], h[11], h[12], h[13]
    dbg_size, ec_size = h[15], h[16]

    # OptionalDbgHeader is the LAST substream; section headers are the int16 at byte 10.
    dbg_off = 64 + modinfo_size + seccontrib + secmap + srcinfo + tsmap + ec_size
    sec_stream = struct.unpack_from('<h', dbi, dbg_off + 10)[0]
    sections = []
    if sec_stream >= 0:
        raw = msf.stream(sec_stream)
        for off in range(0, len(raw) - 39, 40):
            sections.append(struct.unpack_from('<I', raw, off + 12)[0])  # VirtualAddress

    out = []
    p, end, mods = 64, 64 + modinfo_size, 0
    while p + 64 <= end:
        stream_idx = struct.unpack_from('<h', dbi, p + 34)[0]
        sym_size = struct.unpack_from('<I', dbi, p + 36)[0]   # +36, NOT +38
        q = p + 64
        for _ in range(2):                                    # module name, then obj name
            z = dbi.index(b'\0', q)
            q = z + 1
        p = (q + 3) & ~3
        mods += 1
        if stream_idx < 0 or sym_size < 4 or stream_idx >= len(msf.streams):
            continue
        sym = msf.stream(stream_idx)[:sym_size]
        i = 4
        while i + 4 <= len(sym):
            ln = struct.unpack_from('<H', sym, i)[0]
            if ln < 2:
                break
            kind = struct.unpack_from('<H', sym, i + 2)[0]
            if kind in (0x1110, 0x110F) and i + 39 <= len(sym):
                codesize = struct.unpack_from('<I', sym, i + 16)[0]
                coff = struct.unpack_from('<I', sym, i + 32)[0]
                seg = struct.unpack_from('<H', sym, i + 36)[0]
                # +38 is a one-byte FLAGS field; the name starts at +39. The flags byte is
                # often 0x80 but it can be 0x00, and searching for the terminator from +38
                # then finds the flags byte itself -- every such function comes back unnamed.
                z = sym.index(b'\0', i + 39)
                name = sym[i + 39:z].decode('utf-8', 'replace')
                if 1 <= seg <= len(sections):
                    out.append((sections[seg - 1] + coff, codesize, name))
            i += ln + 2
    out.sort()
    return out, mods


def main():
    pdb, prof, module = sys.argv[1], sys.argv[2], sys.argv[3]
    funcs, mods = functions(pdb)
    starts = [f[0] for f in funcs]
    sys.stderr.write('pdb: %d modules, %d procs\n' % (mods, len(funcs)))

    hits, total = [], 0
    for line in open(prof, encoding='utf-8'):
        if line.startswith('#'):
            continue
        p = line.split()
        if len(p) != 3:
            continue
        total += int(p[2])
        if p[0].lower() == module.lower():
            hits.append((int(p[1], 16), int(p[2])))

    agg = {}
    unresolved = 0
    for rva, count in hits:
        j = bisect.bisect_right(starts, rva) - 1
        # Confirm the address is INSIDE the range; the nearest preceding proc will
        # otherwise confidently name the wrong function.
        if j >= 0 and rva < funcs[j][0] + max(funcs[j][1], 1):
            agg[funcs[j][2]] = agg.get(funcs[j][2], 0) + count
        else:
            unresolved += count

    print('%-9s %-7s  %s' % ('samples', 'of all', 'function'))
    for name, c in sorted(agg.items(), key=lambda kv: -kv[1])[:25]:
        print('%-9d %6.2f%%  %s' % (c, 100.0 * c / total, name))
    if unresolved:
        print('%-9d %6.2f%%  (no enclosing S_GPROC32 range)' % (unresolved, 100.0 * unresolved / total))


main()
