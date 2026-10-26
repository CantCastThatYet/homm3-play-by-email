"""hd_port.py OLD.dll NEW.dll [rva ...]: where HD_HOTA.dll addresses the patch uses moved to in another HD build.

The patch reaches into HD_HOTA.dll at fixed addresses (native/hd_build.hpp). An HD update moves them.
For every address:
  code (.text): the old bytes from that address on, relocated words masked out, must occur exactly
    once in the new .text; the match is the new address.
  data: every instruction in the old code that refers to the address (through a relocation) is found
    in the new code the same way, and the address it refers to there is read; all must agree.
An address with no unique answer is reported as such: look at it by hand before trusting the build.
With no rva arguments it maps everything in native/hd_build.hpp's R22 table (`// rva` lines).
"""
import re
import struct
import sys
from pathlib import Path


class Image:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        b = self.data
        pe = struct.unpack_from('<I', b, 0x3C)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<I', b, opt + 28)[0]
        count = struct.unpack_from('<H', b, pe + 6)[0]
        size = struct.unpack_from('<H', b, pe + 20)[0]
        self.sections = []
        for i in range(count):
            name, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', b, opt + size + i * 40)
            self.sections.append((name.rstrip(b'\0').decode(), va, max(vsize, rsize), raw, rsize))
        reloc_rva, reloc_size = struct.unpack_from('<II', b, opt + 96 + 5 * 8)
        self.relocs = set()
        at, end = self.offset(reloc_rva), self.offset(reloc_rva) + reloc_size
        while at < end:
            page, block = struct.unpack_from('<II', b, at)
            if block < 8:
                break
            for i in range((block - 8) // 2):
                entry = struct.unpack_from('<H', b, at + 8 + 2 * i)[0]
                if entry >> 12 == 3:
                    self.relocs.add(page + (entry & 0xFFF))
            at += block
        text = self.section('.text')
        self.text_rva, self.text_size = text[1], text[4]
        self.text = b[text[3]:text[3] + text[4]]
        self.mask = bytearray(len(self.text))
        for r in self.relocs:
            if self.text_rva <= r < self.text_rva + self.text_size - 3:
                for k in range(4):
                    self.mask[r - self.text_rva + k] = 1
        # Relative calls and jumps change whenever caller and callee move apart: their four
        # displacement bytes are wildcards too (an E8/E9 that is really data only loosens a match).
        for i in range(len(self.text) - 4):
            if self.text[i] in (0xE8, 0xE9) and not self.mask[i]:
                for k in range(1, 5):
                    self.mask[i + k] = 1

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def offset(self, rva):
        for _, va, vsize, raw, rsize in self.sections:
            if va <= rva < va + rsize:
                return raw + rva - va
        raise ValueError(hex(rva))

    def in_text(self, rva):
        return self.text_rva <= rva < self.text_rva + self.text_size

    def pattern(self, start, length):
        """Regex for text[start:start+length] with relocated bytes as wildcards."""
        parts = []
        for i in range(start, start + length):
            parts.append(b'.' if self.mask[i] else re.escape(bytes([self.text[i]])))
        return re.compile(b''.join(parts), re.DOTALL)


def unique(new, old, start, before, after):
    """The one place in new.text matching old.text[start-before : start+after], or None."""
    lo = max(0, start - before)
    for grow in (0, 16, 32, 64, 128):
        a, b = max(0, lo - grow), min(len(old.text), start + after + grow)
        found = [m.start() for m in old.pattern(a, b - a).finditer(new.text)]
        if len(found) == 1:
            return found[0] + (start - a)
        if not found:
            return None
    return None


def map_code(old, new, rva):
    """(new rva, how): an exact unique match, else the shift of the nearest matching code around it."""
    start = rva - old.text_rva
    # Windows that hold the address's own code: bytes before it alone can belong to a neighbour
    # laid out differently. The match must also hold the old bytes at the address itself.
    for before, after in ((0, 24), (0, 48), (8, 24), (16, 24)):
        at = unique(new, old, start, before, after)
        if at is not None and all(old.mask[start + k] or old.text[start + k] == new.text[at + k] for k in range(16)):
            return new.text_rva + at, 'exact'
    # A function whose body occurs more than once (small compiler-made helpers): where its callers
    # call in the new build.
    votes = {}
    for call in re.finditer(b'\xe8', old.text):
        site = call.start()
        if site + 5 > len(old.text) or site + 5 + struct.unpack_from('<i', old.text, site + 1)[0] != start:
            continue
        at = unique(new, old, site, 16, 16)
        if at is not None:
            target = at + 5 + struct.unpack_from('<i', new.text, at + 1)[0]
            votes[target] = votes.get(target, 0) + 1
    if len(votes) == 1:
        (target, n), = votes.items()
        return new.text_rva + target, f'{n} callers agree'
    shifts = []
    for step in range(0x20, 0x1000, 0x20):
        for anchor in (start - step, start + step):
            if 0 <= anchor < len(old.text) - 32:
                at = unique(new, old, anchor, 0, 32)
                if at is not None:
                    shifts.append((abs(anchor - start), at - anchor))
        if len(shifts) >= 4:
            break
    if not shifts:
        return None, 'no anchors'
    shift = min(shifts)[1]
    same = sum(1 for k in range(32) if old.mask[start + k] or old.text[start + k] == new.text[start + shift + k])
    agree = len({d for _, d in shifts[:4]}) == 1
    return new.text_rva + start + shift, f'nearby code moved {shift:+#x}{"" if agree else " (neighbours disagree)"}, {same}/32 bytes equal'


def map_data(old, new, rva):
    target = old.base + rva
    votes = {}
    refs = [r for r in sorted(old.relocs) if old.in_text(r)]
    exact = [r for r in refs if struct.unpack_from('<I', old.text, r - old.text_rva)[0] == target]
    for r in exact or refs:
        value = struct.unpack_from('<I', old.text, r - old.text_rva)[0]
        if not (target - 8 <= value <= target + 8):
            continue
        at = unique(new, old, r - old.text_rva, 12, 12)
        if at is None:
            continue
        moved = struct.unpack_from('<I', new.text, at)[0] - new.base - (value - target)
        votes[moved] = votes.get(moved, 0) + 1
    return votes


def main():
    old, new = Image(sys.argv[1]), Image(sys.argv[2])
    rvas = [int(a, 16) for a in sys.argv[3:]]
    if not rvas:
        table = Path(__file__).resolve().parents[1] / 'hd_build.hpp'
        rvas = [int(m, 16) for m in re.findall(r'0x([0-9a-fA-F]+),\s*// rva', table.read_text())]
    failures = 0
    for rva in rvas:
        if old.in_text(rva):
            moved, how = map_code(old, new, rva)
            line = f'code 0x{rva:06x} -> ' + (f'0x{moved:06x} ({moved - rva:+#x})' if moved is not None else 'NOT FOUND') + ('' if how == 'exact' else f'  REVIEW: {how}')
            failures += how != 'exact'
        else:
            votes = map_data(old, new, rva)
            if len(votes) == 1:
                (moved, n), = votes.items()
                line = f'data 0x{rva:06x} -> 0x{moved:06x} ({moved - rva:+#x}), {n} references agree'
            else:
                failures += 1
                line = f'data 0x{rva:06x} -> ' + ('NO REFERENCES FOUND' if not votes else 'DISAGREE ' + ', '.join(f'0x{k:06x}x{v}' for k, v in votes.items()))
        print(line)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
