"""Native VU1 block generator.

usage: python -I tools/vu1/vu1n_gen.py <out dir> <micro-memory dir or .bin files...> [--heads heads.txt] [--parts N] [--locals]

Reads the captured 16 KB VU1 micro-memories (GOW_VU1_CAPTURA), splits them into basic blocks (same rule as
VU1Interpreter's nativeExtent in ps2_vu1_native.inl) and writes straight-line C++ for each distinct (pc, words) block:
immediate semantics, no cycle model, MAC flags only where they can be observed. Output: vu1n_part<k>.cpp. The exact engine
(ps2_vu1_core.cpp) remains the fallback for every block that is not here and the oracle for GOW_VU1N_VERIFY.
heads.txt (optional): extra entry pcs (hex, one per line), e.g. misses logged by GOW_VU1N_DIAG.
--locals: each block keeps the VU registers it uses in C++ locals (loaded at entry, stored back at every exit) and a block
that branches back to its own start repeats inside its function (VU1NativeAccess::chainTo); needs a runtime with chainTo
(ext branch agent/sw-vu1 and later). Without it the blocks read and write the VU1State directly (same output as before).
--no-fast: no fast variants (range analysis, see Bounds; on by default with --locals).
--no-cycles: no two-block cycles (gen_cycle) and no loop regions; --regions: loops of more blocks in one function
(gen_region; off by default, measured slower, see REGIONS).
"""
import os
import re
import struct
import sys

BRANCH_OPS = {0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}
REV4 = [0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15]
CODE_SIZE = 16384
LOCALS = False   # --locals: register file in C++ locals (Regs) and self-loops; else the VU1State form (older runtimes)
# self-loops (needs LOCALS): a block that branches back to its own start repeats inside its function, the registers
# staying in locals (VU1NativeAccess::chainTo between iterations). Measured and dropped: going on to other successors too,
# from one function per group of blocks (GCC spilled the vector registers across the large functions) or by tail calls
# (chainTo costs about what runNative's dispatch does): no gain on the A57 (docs/switch.md).
LOOPS = True
# two-block cycles (needs LOCALS and LOOPS): gen_cycle. --no-cycles turns it off (A/B measurements).
CYCLES = True
# loops of more blocks (needs CYCLES): gen_region, one function per block of the loop that runs the whole loop (the shadow
# loop at 0x0cd8, the PC zones' loop at 0x1728, ...). Off by default (--regions turns it on): bit-exact, but more qemu
# instructions than runNative's dispatch it saves (ship +7 %, PC zone +6 %; docs/vu1-hle.md "Loop regions").
REGIONS = False
REGION_MAX_BLOCKS = 10
REGION_MAX_PAIRS = 64


def fnv_block(pc, words):
    h = 1469598103934665603
    for w in [pc] + list(words):
        for i in range(4):
            h = ((h ^ ((w >> (i * 8)) & 0xFF)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def is_branch(l, u):
    return not (u >> 31) and (l >> 25) in BRANCH_OPS


def extent(code, pc):
    """Pairs (lower, upper) of the block starting at pc, same rule as nativeExtent()."""
    out = []
    p = pc
    while p + 8 <= CODE_SIZE and len(out) < 64:
        l, u = struct.unpack_from('<II', code, p)
        if l == 0 and u == 0:
            break
        out.append((l, u))
        if (u >> 28) & 1 or (u >> 27) & 1:
            break
        if is_branch(l, u) or (u >> 30) & 1:
            if p + 16 <= CODE_SIZE:
                dl, du = struct.unpack_from('<II', code, p + 8)
                if not (dl == 0 and du == 0):
                    out.append((dl, du))
            break
        p += 8
    return out


def heads_of(code, extra_heads):
    heads = set(extra_heads)
    pairs = []
    for i in range(CODE_SIZE // 8):
        l, u = struct.unpack_from('<II', code, i * 8)
        pairs.append((l, u))
    for i, (l, u) in enumerate(pairs):
        if l == 0 and u == 0:
            continue
        pc = i * 8
        if i == 0 or pairs[i - 1] == (0, 0):
            heads.add(pc)
        if is_branch(l, u):
            op = l >> 25
            if op not in (0x24, 0x25):
                heads.add((pc + 8 + sx(l, 11) * 8) & 0x3FFF)
            heads.add((pc + 16) & 0x3FFF)
        if (u >> 30) & 1:
            heads.add((pc + 16) & 0x3FFF)
        if (u >> 28) & 1 or (u >> 27) & 1:
            heads.add((pc + 8) & 0x3FFF)
    return heads



# ---------------------------------------------------------------------------------------------------------------------
# register file of a block: C++ locals (loaded at block entry, stored back at every exit) or the VU1State in memory.
# The blocks are straight-line, so a local per register is exact; the compiler then keeps the values in NEON / SSE
# registers instead of a store and a reload per instruction (the data-memory stores through n.data may alias the state,
# so it could not do that with s.vf). Blocks that call out to the exact engine (callUpper / callLower) keep the memory form.

class Regs:
    def __init__(self, local):
        self.local = local
        self.need = []        # read before written: loaded at block entry (in first-use order)
        self.defined = []     # every register the block touches
        self.dirty = []       # written so far (stored back at an exit)
        # loop functions (self-loops, gen_cycle): VF registers the function never writes; their operand view (nrm) is
        # computed once at entry (N<r>) instead of at every use in every pass (the matrix rows of the vertex loops)
        self.inv = set()
        self.nused = []

    def _use(self, key):
        if key not in self.defined:
            self.defined.append(key)
            self.need.append(key)

    def _def(self, key):
        if key not in self.defined:
            self.defined.append(key)
        if key not in self.dirty:
            self.dirty.append(key)

    def f(self, r):
        """raw value of VF r"""
        if not self.local:
            return f"vu1n::ld(s, {r})"
        self._use(('f', r))
        return f"F{r}"

    def fn(self, r, clean=None, lanes=15):
        """operand view (Inf/NaN -> +-FLT_MAX) of VF r. vf0 is its own view, and so is a register whose lanes the
        operation uses (lanes: SSE order) are all clean (clean: {vf: SSE lane mask known finite})"""
        if BND is not None and self.local:
            # fast variant: the raw value when the used lanes are known finite (range analysis, Bounds)
            ok, vals = BND.view(r, lanes)
            BND.last = vals
            if ok:
                BND.require_lanes(('f', r), lanes, vals)
                if r != 0:
                    BND.score += 3   # nrm: BIC, UMIN, BSL
                return self.f(r)
            clean = None
        elif r == 0 or (clean and (clean.get(r, 0) & lanes) == lanes):
            return self.f(r)
        if not self.local:
            return f"vu1n::ldn(s, {r})"
        if r in self.inv:
            self.f(r)
            if r not in self.nused:
                self.nused.append(r)
            return f"N{r}"
        return f"vu1n::nrm({self.f(r)})"

    def setf(self, r, m, expr):
        if not self.local:
            return f"vu1n::stf<{m}>(s.vf[{r}], {expr});"
        if m == 0:
            return ""
        if m == 15:
            self._def(('f', r))
            return f"F{r} = {expr};"
        old = self.f(r)
        self._def(('f', r))
        return f"{old} = _mm_blend_ps({old}, {expr}, {m});"

    def vi(self, r):
        if not self.local:
            return f"s.vi[{r}]"
        self._use(('i', r))
        return f"I{r}"

    def setvi(self, r, expr):
        if not self.local:
            return f"s.vi[{r}] = {expr};"
        self._def(('i', r))
        return f"I{r} = {expr};"

    def acc(self):
        if not self.local:
            return "vu1n::ldAcc(s)"
        self._use(('acc',))
        return "ACC"

    def setacc(self, m, expr):
        if not self.local:
            return f"vu1n::stf<{m}>(s.acc, {expr});"
        if m == 15:
            self._def(('acc',))
            return f"ACC = {expr};"
        old = self.acc()
        self._def(('acc',))
        return f"ACC = _mm_blend_ps(ACC, {expr}, {m});"

    def q(self):
        if not self.local:
            return "s.q"
        self._use(('q',))
        return "QR"

    def setq(self, expr):
        if not self.local:
            return f"s.q = {expr};"
        self._def(('q',))
        return f"QR = {expr};"

    def ireg(self):
        if not self.local:
            return "s.i"
        self._use(('I',))
        return "IR"

    def setireg(self, expr):
        if not self.local:
            return f"s.i = {expr};"
        self._def(('I',))
        return f"IR = {expr};"

    @staticmethod
    def _name(key):
        return {'f': f"F{key[-1]}", 'i': f"I{key[-1]}", 'acc': "ACC", 'q': "QR", 'I': "IR"}[key[0]]

    def decls(self):
        out = []
        for key in self.defined:
            name = self._name(key)
            load = key in self.need
            if key[0] == 'f':
                out.append(f"__m128 {name}" + (f" = vu1n::ld(s, {key[1]});" if load else ";"))
            elif key[0] == 'i':
                out.append(f"int32_t {name}" + (f" = s.vi[{key[1]}];" if load else " = 0;"))
            elif key[0] == 'acc':
                out.append("__m128 ACC" + (" = vu1n::ldAcc(s);" if load else ";"))
            elif key[0] == 'q':
                out.append("float QR" + (" = s.q;" if load else " = 0.0f;"))
            else:
                out.append("float IR" + (" = s.i;" if load else " = 0.0f;"))
        for r in self.nused:
            out.append(f"const __m128 N{r} = vu1n::nrm(F{r});")
        return out

    def writeback(self):
        out = []
        for key in self.dirty:
            name = self._name(key)
            if key[0] == 'f':
                out.append(f"_mm_storeu_ps(s.vf[{key[1]}], {name});")
            elif key[0] == 'i':
                out.append(f"s.vi[{key[1]}] = {name};")
            elif key[0] == 'acc':
                out.append("_mm_storeu_ps(s.acc, ACC);")
            elif key[0] == 'q':
                out.append("s.q = QR;")
            else:
                out.append("s.i = IR;")
        return out


RG = Regs(False)


# ---------------------------------------------------------------------------------------------------------------------
# fast variants (docs/vu1-hle.md "Exact float handling out of the hot loops"): range analysis on the generated code.
# The native blocks already run in the VU's float mode (round toward zero, flush to zero: runNative sets FPCR / MXCSR), so
# a plain float op gives the exact engine's result for finite operands. What is left of the exact handling is the operand
# view (nrm: Inf/NaN -> +-FLT_MAX), the clamp after ADD/SUB/MUL (the identity for finite operands: round toward zero never
# overflows to infinity) and MADD/MSUB's overflow test (maddc: a product or sum at FLT_MAX in any of the four lanes takes
# the wide path). Bounds tracks for every VF lane, ACC lane, Q and I a bound |x| < 2^e (None: unknown, may be Inf/NaN or
# integer bits) through the generated code; where the bounds prove the exact handling is the identity the fast variant
# emits the plain op:
# - operand view of lanes known finite: the raw register;
# - ADD/SUB/MUL with finite operands: no clamp; CLIP in vector compares; FTOI without its clamp;
# - MADD/MSUB whose product and sum stay below 2^127 in all four lanes (as maddc tests them): acc +/- a*b, no test.
# The function's entry values get an assumed bound 2^E per lane (origin); the fast code records which origins it relied on
# and how (the bound, or only being finite: every value the analysis knows is finite when its origins are), and its entry
# check tests exactly those, else the exact function (<name>_x, the code as before) runs. Loops: fixed point of the
# loop-carried bounds (join with the entry state). E per origin: _search. Functions: self-loops, gen_cycle, and plain
# blocks when the saving exceeds the check (fast_straight). Bit-exact by construction: each change is an identity under
# the proven bounds.
FAST = True
# plain MADD decided on all four lanes, as maddc tests them (VU1N_FAST_DEST=1: on the DEST lanes only, as the exact engine
# does; 282 of the 1832 PC zone-walk records then differ from the native blocks: a huge or NaN lane outside DEST sends
# maddc to the wide path, whose single rounding gives other DEST values than the two-step fast path)
FAST_DEST_ONLY = os.environ.get('VU1N_FAST_DEST', '0') == '1'
FAST_CLIPV = os.environ.get('VU1N_FAST_CLIPV', '1') == '1'
ZERO_E = -1000            # bound of an exact zero
BND = None                # Bounds while a fast variant is generated
LAST = {}                 # gen_block / gen_cycle: facts about the last generated function (self_loop, decls)


def _join(a, b):
    if a is None or b is None:
        return None
    return (max(a[0], b[0]), a[1] | b[1])


def _f32_e(bits):
    """bound exponent of a float constant after qnorm"""
    ex = (bits >> 23) & 0xFF
    if ex == 0:
        return (ZERO_E, frozenset())
    if ex == 255:
        return (128, frozenset())
    return (ex - 127 + 1, frozenset())


class Bounds:
    def __init__(self, start):
        self.st = {k: (list(v) if isinstance(v, list) else v) for k, v in start.items()}
        self.req = {}     # origin -> 2: its bound 2^E was relied on (plain MADD), 1: only that it is finite
        self.score = 0
        self.fails = []   # origins of MADDs left on maddc although every lane is known (_search lowers their bounds)
        self.last = None

    @staticmethod
    def entry(Emap, Edef):
        def v(o):
            return (Emap.get(o, Edef), frozenset({o}))
        st = {}
        for r in range(32):
            st[('f', r)] = [v(('f', r, k)) for k in range(4)]
        st[('acc',)] = [v(('acc', k)) for k in range(4)]
        st[('q',)] = v(('q',))
        st[('I',)] = v(('I',))
        return st

    @staticmethod
    def join_states(a, b):
        out = {}
        for k in a:
            if isinstance(a[k], list):
                out[k] = [_join(x, y) for x, y in zip(a[k], b[k])]
            else:
                out[k] = _join(a[k], b[k])
        return out

    @staticmethod
    def widen(old, new):
        """lanes whose bound still grows go to the float range (|x| <= FLT_MAX < 2^128)"""
        def w(o, n):
            if o is None or n is None:
                return n
            return (128, n[1]) if n[0] > o[0] else n
        out = {}
        for k in new:
            if isinstance(new[k], list):
                out[k] = [w(x, y) for x, y in zip(old[k], new[k])]
            else:
                out[k] = w(old[k], new[k])
        return out

    def lanes(self, key):
        return self.st[key]

    def view(self, r, lanes):
        """(raw ok, per-lane values of the operand view): ok when every used lane is known; else the view is nrm()"""
        v = self.st[('f', r)]
        ok = all(v[k] is not None for k in range(4) if lanes & (1 << k))
        if ok:
            return True, list(v)
        return False, [x if x is not None else (128, frozenset()) for x in v]

    def require(self, val, level=2):
        """the code relies on val's bound (level 2) or only on its being finite (level 1): a value known to the analysis
        is finite whenever its origins are (round toward zero never overflows to infinity), whatever their magnitude"""
        if val is not None:
            for o in val[1]:
                if self.req.get(o, 0) < level:
                    self.req[o] = level

    def require_lanes(self, key, lanes, vals):
        for k in range(4):
            if lanes & (1 << k):
                self.require(vals[k], 1)

    def setv(self, key, m, vals):
        cur = self.st[key]
        self.st[key] = [vals[k] if m & (1 << k) else cur[k] for k in range(4)]




# ---------------------------------------------------------------------------------------------------------------------
# upper (FMAC) decode

def classify(u):
    """(kind, source, toAcc, bc) like vu1c::classify, plus special kinds."""
    op = u & 0x3F
    if op < 0x3C:
        c = op
        if c <= 0x03: return ('add', 'bc', False, c & 3)
        if c <= 0x07: return ('sub', 'bc', False, c & 3)
        if c <= 0x0B: return ('madd', 'bc', False, c & 3)
        if c <= 0x0F: return ('msub', 'bc', False, c & 3)
        if c <= 0x13: return ('max', 'bc', False, c & 3)
        if c <= 0x17: return ('mini', 'bc', False, c & 3)
        if c <= 0x1B: return ('mul', 'bc', False, c & 3)
        table = {0x1C: ('mul', 'q'), 0x1D: ('max', 'i'), 0x1E: ('mul', 'i'), 0x1F: ('mini', 'i'),
                 0x20: ('add', 'q'), 0x21: ('madd', 'q'), 0x22: ('add', 'i'), 0x23: ('madd', 'i'),
                 0x24: ('sub', 'q'), 0x25: ('msub', 'q'), 0x26: ('sub', 'i'), 0x27: ('msub', 'i'),
                 0x28: ('add', 'v'), 0x29: ('madd', 'v'), 0x2A: ('mul', 'v'), 0x2B: ('max', 'v'),
                 0x2C: ('sub', 'v'), 0x2D: ('msub', 'v'), 0x2E: ('opmsub', 'v'), 0x2F: ('mini', 'v')}
        if c in table:
            return (table[c][0], table[c][1], False, 0)
        return ('other', 'v', False, 0)
    sp = (u & 3) | ((u >> 4) & 0x7C)
    if sp <= 0x03: return ('add', 'bc', True, sp & 3)
    if sp <= 0x07: return ('sub', 'bc', True, sp & 3)
    if sp <= 0x0B: return ('madd', 'bc', True, sp & 3)
    if sp <= 0x0F: return ('msub', 'bc', True, sp & 3)
    if 0x10 <= sp <= 0x13: return ('itof', (0, 4, 12, 15)[sp & 3], False, 0)
    if 0x14 <= sp <= 0x17: return ('ftoi', (0, 4, 12, 15)[sp & 3], False, 0)
    if 0x18 <= sp <= 0x1B: return ('mul', 'bc', True, sp & 3)
    sp_table = {0x1C: ('mul', 'q'), 0x1E: ('mul', 'i'), 0x20: ('add', 'q'), 0x21: ('madd', 'q'), 0x22: ('add', 'i'),
                0x23: ('madd', 'i'), 0x24: ('sub', 'q'), 0x25: ('msub', 'q'), 0x26: ('sub', 'i'), 0x27: ('msub', 'i'),
                0x28: ('add', 'v'), 0x29: ('madd', 'v'), 0x2A: ('mul', 'v'), 0x2C: ('sub', 'v'), 0x2D: ('msub', 'v'),
                0x2E: ('opmula', 'v')}
    if sp == 0x1D: return ('abs', None, False, 0)
    if sp == 0x1F: return ('clip', None, False, 0)
    if sp in (0x2F, 0x30): return ('nop', None, False, 0)
    if sp in sp_table:
        return (sp_table[sp][0], sp_table[sp][1], True, 0)
    return ('other', 'v', False, 0)


def upper_info(u):
    """(writes_vf set, reads_vf set, writes_mac, reads_q)"""
    kind, src, toacc, bc = classify(u)
    dest = (u >> 21) & 0xF
    ft, fs, fd = (u >> 16) & 31, (u >> 11) & 31, (u >> 6) & 31
    w, r = set(), set()
    if kind == 'nop' or dest == 0 and kind not in ('clip',):
        return w, r, False, False
    if kind in ('add', 'sub', 'madd', 'msub', 'mul', 'max', 'mini'):
        r.add(fs)
        if src in ('bc', 'v'):
            r.add(ft)
        if not toacc:
            w.add(fd)
    elif kind in ('itof', 'ftoi', 'abs'):
        r.add(fs)
        w.add(ft)
    elif kind == 'clip':
        r.update((fs, ft))
    elif kind in ('opmsub', 'opmula', 'other'):
        r.update((fs, ft))
        if not toacc:
            w.add(fd)
    writes_mac = kind in ('add', 'sub', 'madd', 'msub', 'mul') and dest != 0
    reads_q = src == 'q'
    return {x for x in w if x}, r, writes_mac, reads_q


def emit_upper(u, pc, idx, mac_var, clip_var, prev_clip, clean=None):
    kind, src, toacc, bc = classify(u)
    dest = (u >> 21) & 0xF
    ft, fs, fd = (u >> 16) & 31, (u >> 11) & 31, (u >> 6) & 31
    m = REV4[dest]
    L = []
    if kind == 'nop':
        return L
    if kind == 'clip':
        if not RG.local:
            return [f"const uint32_t {clip_var} = vu1n::clipVal(s, {fs}, {ft}, {prev_clip});"]
        if BND is not None and FAST_CLIPV:
            return [f"const uint32_t {clip_var} = vu1nf::clipv({RG.fn(fs)}, {RG.fn(ft)}, {prev_clip});"]
        return [f"const uint32_t {clip_var} = vu1n::clipValv({RG.fn(fs)}, {RG.fn(ft)}, {prev_clip});"]
    if dest == 0:
        return L
    if kind == 'itof':
        if not ft:
            return L
        if not RG.local:
            return [f"vu1n::itof<{src}, {m}>(s, {fs}, {ft});"]
        line = RG.setf(ft, m, f"vu1n::itofv<{src}>({RG.f(fs)})")
        if BND is not None:
            BND.setv(('f', ft), m, [(32 - src, frozenset())] * 4)  # |int32| <= 2^31, scaled by 2^-src
        return [line]
    if kind == 'ftoi':
        if not ft:
            return L
        if not RG.local:
            return [f"vu1n::ftoi<{src}, {m}>(s, {fs}, {ft});"]
        if BND is not None:
            line = RG.setf(ft, m, f"vu1nf::ftoiv<{src}>({RG.fn(fs, clean, m)})")
            BND.setv(('f', ft), m, [None] * 4)  # integer bits
            return [line]
        return [RG.setf(ft, m, f"vu1n::ftoiv<{src}>({RG.fn(fs, clean, m)})")]
    if kind == 'abs':
        if not ft:
            return L
        if not RG.local:
            return [f"vu1n::absOp<{m}>(s, {fs}, {ft});"]
        line = RG.setf(ft, m, f"vu1n::absv({RG.fn(fs, clean, m)})")
        if BND is not None:
            BND.setv(('f', ft), m, BND.last)
        return [line]
    if kind in ('opmsub', 'opmula', 'other'):
        assert not RG.local
        return [f"VU1NativeAccess::callUpper(n, {pc}u, 0x{u:08x}u);"]
    # only the DEST lanes of a result are kept and flagged, and a broadcast reads one lane: those are the lanes an
    # operand needs normalised
    def opnd(r, lanes):
        return RG.fn(r, clean, lanes)
    a = opnd(fs, m)
    va = BND.last if BND is not None else None
    vb = None
    if src == 'bc':
        b = f"vu1n::bc<{bc}>({opnd(ft, 1 << bc)})"
        if BND is not None:
            vb = [BND.last[bc]] * 4
    elif src == 'q':
        b = f"_mm_set1_ps({RG.q()})"
        if BND is not None:
            vb = [BND.st[('q',)]] * 4
    elif src == 'i':
        b = f"_mm_set1_ps({RG.ireg()})"
        if BND is not None:
            vb = [BND.st[('I',)]] * 4
    else:
        b = opnd(ft, m)
        if BND is not None:
            vb = BND.last
    acc = RG.acc() if kind in ('madd', 'msub') else None
    if BND is not None:
        return emit_upper_fast(kind, src, toacc, m, fd, idx, mac_var, a, b, acc, va, vb)
    if kind == 'add':
        r = f"_mm_add_ps({a}, {b})"
    elif kind == 'sub':
        r = f"_mm_sub_ps({a}, {b})"
    elif kind == 'mul':
        r = f"_mm_mul_ps({a}, {b})"
    elif kind == 'madd':
        r = f"vu1n::madd<false>({acc}, {a}, {b})"
    elif kind == 'msub':
        r = f"vu1n::madd<true>({acc}, {a}, {b})"
    elif kind == 'max':
        r = f"_mm_max_ps({a}, {b})"
    else:
        r = f"_mm_min_ps({a}, {b})"
    if kind in ('madd', 'msub'):
        # maddc: clamps only on its wide path (the fast path's result is already in range); q = the unclamped result
        out = [f"__m128 q{idx};", f"__m128 r{idx} = vu1n::maddc<{'true' if kind == 'msub' else 'false'}>({acc}, {a}, {b}, q{idx});"]
        if mac_var:
            out.append(f"const uint32_t {mac_var} = vu1n::macVal<{m}>(q{idx});")
    else:
        out = [f"__m128 r{idx} = {r};"]
        if kind in ('add', 'sub', 'mul') and mac_var:
            out.append(f"const uint32_t {mac_var} = vu1n::macVal<{m}>(r{idx});")
            out.append(f"const __m128 q{idx} = r{idx};  // unclamped: flags computed later if it outlives the block")
        if kind not in ('max', 'min', 'mini'):
            out.append(f"r{idx} = vu1n::clampv(r{idx});")
    if toacc:
        out.append(RG.setacc(m, f"r{idx}"))
    elif fd:
        out.append(RG.setf(fd, m, f"r{idx}"))
    return out


def emit_upper_fast(kind, src, toacc, m, fd, idx, mac_var, a, b, acc, va, vb):
    """ADD/SUB/MUL/MADD/MSUB/MAX/MINI of a fast variant (Bounds): operands a, b (views va, vb per lane), raw acc"""
    lm = [k for k in range(4) if m & (1 << k)]
    res = [None] * 4
    if kind in ('madd', 'msub'):
        vacc = BND.st[('acc',)]
        # per DEST lane, as the exact engine (normalizeFmacResult) and the hardware: the other lanes are not written and
        # their MAC bits are masked off (maddc's test covers all four lanes, so a huge or NaN lane outside DEST sends it to
        # the wide path; with every DEST lane in range that gives the same DEST values unless the two-step and the wide
        # rounding differ there)
        lm4 = lm if FAST_DEST_ONLY else range(4)
        ok = all(va[k] is not None and vb[k] is not None and vacc[k] is not None for k in lm4)
        if ok:
            culprit = frozenset()
            for k in lm4:
                pe = va[k][0] + vb[k][0]
                re = max(vacc[k][0], pe) + 1
                if pe > 127 or re > 127:
                    ok = False
                    # the operands that make it overflow: the product's, or ACC's when it dominates the sum
                    culprit |= va[k][1] | vb[k][1] if pe > 127 or pe >= vacc[k][0] else vacc[k][1]
                res[k] = (re, va[k][1] | vb[k][1] | vacc[k][1])
            if not ok:
                BND.fails.append(culprit)
                res = [None] * 4
        sub = kind == 'msub'
        if ok:
            # product and sum below 2^127 in every lane: maddc's fast path, which is these two ops
            for k in lm4:
                BND.require(va[k]); BND.require(vb[k]); BND.require(vacc[k])
            BND.score += 7   # maddc's test: two BIC, UMAX, UMAXV, FMOV, CMP, branch
            out = [f"__m128 r{idx} = _mm_{'sub' if sub else 'add'}_ps({acc}, _mm_mul_ps({a}, {b}));",
                   f"const __m128 q{idx} = r{idx};"]
        else:
            res = [(128, frozenset())] * 4   # maddc: below FLT_MAX or clamped
            out = [f"__m128 q{idx};", f"__m128 r{idx} = vu1n::maddc<{'true' if sub else 'false'}>({acc}, {a}, {b}, q{idx});"]
        if mac_var:
            out.append(f"const uint32_t {mac_var} = vu1n::macVal<{m}>(q{idx});")
    elif kind in ('add', 'sub', 'mul'):
        op = {'add': '_mm_add_ps', 'sub': '_mm_sub_ps', 'mul': '_mm_mul_ps'}[kind]
        out = [f"__m128 r{idx} = {op}({a}, {b});"]
        if mac_var:
            out.append(f"const uint32_t {mac_var} = vu1n::macVal<{m}>(r{idx});")
            out.append(f"const __m128 q{idx} = r{idx};  // unclamped: flags computed later if it outlives the block")
        # the views a, b are finite; a Q / I operand only when its bound is known
        fin = all(vb[k] is not None for k in lm)
        if fin:
            if src in ('q', 'i'):
                for k in lm:
                    BND.require(vb[k], 1)
            BND.score += 2   # clampv: FMAXNM, FMINNM
            for k in lm:
                e = va[k][0] + vb[k][0] if kind == 'mul' else max(va[k][0], vb[k][0]) + 1
                res[k] = (min(e, 128), va[k][1] | vb[k][1])   # round toward zero: finite operands, finite result
        else:
            out.append(f"r{idx} = vu1n::clampv(r{idx});")
            res = [(128, frozenset())] * 4
    else:
        op = '_mm_max_ps' if kind == 'max' else '_mm_min_ps'
        out = [f"__m128 r{idx} = {op}({a}, {b});"]
        for k in lm:
            if va[k] is not None and vb[k] is not None:
                res[k] = (max(va[k][0], vb[k][0]), va[k][1] | vb[k][1])
    if toacc:
        out.append(RG.setacc(m, f"r{idx}"))
        BND.setv(('acc',), m, res)
    elif fd:
        out.append(RG.setf(fd, m, f"r{idx}"))
        BND.setv(('f', fd), m, res)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# lower decode

def lower_fields(l):
    return dict(dest=(l >> 21) & 0xF, ft=(l >> 16) & 31, fs=(l >> 11) & 31, vid=(l >> 6) & 31,
                imm11=sx(l, 11), imm15=(l & 0x7FF) | ((l >> 10) & 0x7800), imm24=l & 0xFFFFFF)


def lower_special(l):
    return ((l & 3) | ((l >> 4) & 0x7C)) if (l & 0x3F) >= 0x3C else None


def lower_info(l):
    """Describe a lower instruction.

    returns dict: vfw set, vfr set, reads_mac, reads_clip, writes_q, vi_delay_reg (reg whose write delays a following
    branch read), native (bool), branch (kind or None), vi_reads set"""
    op = l >> 25
    f = lower_fields(l)
    info = dict(vfw=set(), vfr=set(), reads_mac=False, reads_clip=False, writes_q=False, delay=0, native=True,
                branch=None, calls_out=False, vi_reads=set())
    if l == 0 or l == 0x8000033C:
        return info
    ft, fs, vid = f['ft'], f['fs'], f['vid']
    if op == 0x00:
        info['vfw'] = {ft}
        info['vi_reads'] = {fs}
    elif op == 0x01:
        info['vfr'] = {fs}
        info['vi_reads'] = {ft}
    elif op in (0x04, 0x05):
        info['vi_reads'] = {fs, ft} if op == 0x05 else {fs}
    elif op in (0x08, 0x09):
        info['delay'] = ft
        info['vi_reads'] = {fs}
    elif op in (0x10, 0x11, 0x12, 0x13, 0x1C):
        info['reads_clip'] = op != 0x11
    elif op in (0x18, 0x1A, 0x1B):
        info['reads_mac'] = True
        info['vi_reads'] = {fs}
    elif op in BRANCH_OPS:
        info['branch'] = op
        info['vi_reads'] = {fs, ft} if op in (0x28, 0x29) else ({fs} if op in (0x24, 0x25, 0x2C, 0x2D, 0x2E, 0x2F) else set())
    elif op == 0x40:
        fn = l & 0x3F
        if fn in (0x30, 0x31, 0x34, 0x35):
            info['delay'] = vid
            info['vi_reads'] = {fs, ft}
        elif fn == 0x32:
            info['delay'] = ft
            info['vi_reads'] = {fs}
        elif fn >= 0x3C:
            sp = lower_special(l)
            if sp in (0x30, 0x31):
                info['vfw'] = {ft}
                info['vfr'] = {fs}
            elif sp in (0x34, 0x36):
                info['vfw'] = {ft}
                info['delay'] = fs
                info['vi_reads'] = {fs}
            elif sp in (0x35, 0x37):
                info['vfr'] = {fs}
                info['delay'] = ft
                info['vi_reads'] = {ft}
            elif sp == 0x38:
                info['vfr'] = {fs, ft}
                info['writes_q'] = True
            elif sp == 0x39:
                info['vfr'] = {ft}
                info['writes_q'] = True
            elif sp == 0x3A:
                info['vfr'] = {fs, ft}
                info['writes_q'] = True
            elif sp == 0x3B:
                pass
            elif sp == 0x3C:
                info['vfr'] = {fs}
                info['delay'] = ft
            elif sp == 0x3D:
                info['vfw'] = {ft}
                info['vi_reads'] = {fs}
            elif sp in (0x3E, 0x3F):
                info['vi_reads'] = {fs, ft}
            elif sp == 0x64:
                info['vfw'] = {ft}
            elif sp in (0x68, 0x69, 0x6C):
                info['vi_reads'] = {fs} if sp == 0x6C else set()
            else:
                info['native'] = False
                info['calls_out'] = True
                info['vfr'] = {fs, ft}
                info['vfw'] = {ft}
        else:
            info['native'] = False
            info['calls_out'] = True
    else:
        info['native'] = False
        info['calls_out'] = True
        info['vfr'] = {fs, ft}
        info['vfw'] = {ft}
    info['vfw'] = {x for x in info['vfw'] if x}
    return info


def dest_comp(dest):
    return 0 if dest & 8 else 1 if dest & 4 else 2 if dest & 2 else 3


def emit_lower(l, pc, upper, bkp, mac_expr, clip_expr, clip_var):
    """Lines for a non-branch lower op. bkp: dict reg->variable name for branch backups (unused here)."""
    op = l >> 25
    f = lower_fields(l)
    dest, ft, fs, vid, imm11 = f['dest'], f['ft'], f['fs'], f['vid'], f['imm11']
    m = REV4[dest]
    if l == 0 or l == 0x8000033C:
        return []
    L = []
    V, VN, SETF, VI = RG.f, RG.fn, RG.setf, RG.vi
    def vi_set(reg, expr):
        return [RG.setvi(reg, expr)] if reg else []
    def unknown(reg):
        if BND is not None:
            BND.setv(('f', reg), m, [None] * 4)
    def sq(addr, reg):
        if not RG.local:
            return f"vu1n::stq<{m}>(n, {addr}, vu1n::ld(s, {reg}));"
        return f"vu1n::stq<{m}>(n, {addr}, {V(reg)});"
    if op == 0x00:
        if ft:
            L.append(f"{{ uint32_t a = vu1n::qaddr(n, {VI(fs)} + {imm11}); {SETF(ft, m, 'vu1n::ldq(n, a)')} }}")
            unknown(ft)
        return L
    if op == 0x01:
        return [f"{{ uint32_t a = vu1n::qaddr(n, {VI(ft)} + {imm11}); {sq('a', fs)} }}"]
    if op == 0x04:
        return vi_set(ft, f"vu1n::ldWord(n, vu1n::qaddr(n, {VI(fs)} + {imm11}), {dest_comp(dest)})")
    if op == 0x05:
        return [f"vu1n::stWords(n, vu1n::qaddr(n, {VI(fs)} + {imm11}), {VI(ft)}, {dest});"]
    if op == 0x08:
        return vi_set(ft, f"vu1n::sx16({VI(fs)} + {f['imm15']})")
    if op == 0x09:
        return vi_set(ft, f"vu1n::sx16({VI(fs)} - {f['imm15']})")
    if op == 0x10:
        return vi_set(1, f"(({clip_expr} & 0xFFFFFFu) == 0x{f['imm24']:x}u) ? 1 : 0")
    if op == 0x11:
        return [f"const uint32_t {clip_var} = 0x{f['imm24']:x}u;"]
    if op == 0x12:
        return vi_set(1, f"(({clip_expr} & 0x{f['imm24']:x}u) != 0u) ? 1 : 0")
    if op == 0x13:
        return vi_set(1, f"(({clip_expr} | 0x{f['imm24']:x}u) == 0xFFFFFFu) ? 1 : 0")
    if op == 0x18:
        return vi_set(ft, f"(({mac_expr} & 0xFFFFu) == (uint32_t)(uint16_t){VI(fs)}) ? 1 : 0")
    if op == 0x1A:
        return vi_set(ft, f"(int32_t)({mac_expr} & (uint32_t)(uint16_t){VI(fs)})")
    if op == 0x1B:
        return vi_set(ft, f"(int32_t)({mac_expr} | (uint32_t)(uint16_t){VI(fs)})")
    if op == 0x1C:
        return vi_set(ft, f"(int32_t)({clip_expr} & 0x0FFFu)")
    if op == 0x40:
        fn = l & 0x3F
        if fn == 0x30:
            return vi_set(vid, f"vu1n::sx16({VI(fs)} + {VI(ft)})")
        if fn == 0x31:
            return vi_set(vid, f"vu1n::sx16({VI(fs)} - {VI(ft)})")
        if fn == 0x32:
            return vi_set(ft, f"vu1n::sx16({VI(fs)} + {sx((l >> 6) & 0x1F, 5)})")
        if fn == 0x34:
            return vi_set(vid, f"{VI(fs)} & {VI(ft)}")
        if fn == 0x35:
            return vi_set(vid, f"{VI(fs)} | {VI(ft)}")
        sp = lower_special(l)
        if sp == 0x30:
            if ft and BND is not None:
                src_v = BND.st[('f', fs)]
                line = SETF(ft, m, V(fs))
                BND.setv(('f', ft), m, list(src_v))
                return [line]
            return [SETF(ft, m, V(fs))] if ft else []
        if sp == 0x31:
            if not ft:
                return []
            src_v = BND.st[('f', fs)] if BND is not None else None
            line = f"{{ __m128 v = {V(fs)}; {SETF(ft, m, '_mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 3, 2, 1))')} }}"
            if BND is not None:
                BND.setv(('f', ft), m, [src_v[(k + 1) % 4] for k in range(4)])
            return [line]
        if sp == 0x34:  # LQI
            out = []
            if ft:
                out.append(f"{{ uint32_t a = vu1n::qaddr(n, (uint16_t){VI(fs)}); {SETF(ft, m, 'vu1n::ldq(n, a)')} }}")
                unknown(ft)
            if fs:
                out.append(RG.setvi(fs, f"vu1n::sx16({VI(fs)} + 1)"))
            return out
        if sp == 0x35:  # SQI
            out = [f"{{ uint32_t a = vu1n::qaddr(n, (uint16_t){VI(ft)}); {sq('a', fs)} }}"]
            if ft:
                out.append(RG.setvi(ft, f"vu1n::sx16({VI(ft)} + 1)"))
            return out
        if sp == 0x36:  # LQD
            out = []
            if fs:
                out.append(RG.setvi(fs, f"vu1n::sx16({VI(fs)} - 1)"))
            if ft:
                out.append(f"{{ uint32_t a = vu1n::qaddr(n, (uint16_t){VI(fs)}); {SETF(ft, m, 'vu1n::ldq(n, a)')} }}")
                unknown(ft)
            return out
        if sp == 0x37:  # SQD
            out = []
            if ft:
                out.append(RG.setvi(ft, f"vu1n::sx16({VI(ft)} - 1)"))
            out.append(f"{{ uint32_t a = vu1n::qaddr(n, (uint16_t){VI(ft)}); {sq('a', fs)} }}")
            return out
        fsf, ftf = (l >> 21) & 3, (l >> 23) & 3
        if sp in (0x38, 0x39, 0x3A) and BND is not None:
            BND.st[('q',)] = (128, frozenset())  # qfinish: finite
        if sp == 0x38:
            if not RG.local:
                return [f"vu1n::divOp(s, {fs}, {fsf}, {ft}, {ftf});"]
            return [RG.setq(f"vu1n::divv(vu1n::lane<{fsf}>({V(fs)}), vu1n::lane<{ftf}>({V(ft)}))")]
        if sp == 0x39:
            if not RG.local:
                return [f"vu1n::sqrtOp(s, {ft}, {ftf});"]
            return [RG.setq(f"vu1n::sqrtv(vu1n::lane<{ftf}>({V(ft)}))")]
        if sp == 0x3A:
            if not RG.local:
                return [f"vu1n::rsqrtOp(s, {fs}, {fsf}, {ft}, {ftf});"]
            return [RG.setq(f"vu1n::rsqrtv(vu1n::lane<{fsf}>({V(fs)}), vu1n::lane<{ftf}>({V(ft)}))")]
        if sp == 0x3B:
            return []
        if sp == 0x3C:
            if not RG.local:
                return vi_set(ft, f"vu1n::compBits(s, {fs}, {fsf})")
            return vi_set(ft, f"vu1n::laneBits16<{fsf}>({V(fs)})")
        if sp == 0x3D:
            if ft:
                unknown(ft)
            return [SETF(ft, m, f"vu1n::fromInt(vu1n::sx16({VI(fs)}))")] if ft else []
        if sp == 0x3E:
            return vi_set(ft, f"vu1n::ldWord(n, vu1n::qaddr(n, (uint16_t){VI(fs)}), {dest_comp(dest)})")
        if sp == 0x3F:
            return [f"vu1n::stWords(n, vu1n::qaddr(n, (uint16_t){VI(fs)}), {VI(ft)}, {dest});"]
        if sp == 0x64:
            if ft:
                unknown(ft)
            return [SETF(ft, m, "_mm_set1_ps(s.p)")] if ft else []
        if sp == 0x68:
            return vi_set(ft, "(int32_t)(s.top & 0x3FFu)")
        if sp == 0x69:
            return vi_set(ft, "(int32_t)(s.itop & 0x3FFu)")
        if sp == 0x6C:
            return [f"VU1NativeAccess::xgkick(n, (uint32_t)(uint16_t){VI(fs)});"]
    assert not RG.local
    return [f"VU1NativeAccess::callLower(n, {pc}u, 0x{l:08x}u, 0x{upper:08x}u);"]


def gen_block(pc0, pairs, tail_out=None, selfname=None, regs=None, pieces=None):
    """tail_out: list that receives the block's VU1TailDesc (mac pairs, mac masks, clip pairs) when it has one.
    selfname: the block's own function name when it may repeat itself in place (self-loop, see the exit below).
    regs / pieces (gen_cycle): generate with that register file and return only the body lines and the exit expression
    in pieces (lines, nx, end_pc) instead of a whole function.
    C++ body for a block, or None if the block uses something the native backend does not support."""
    n = len(pairs)
    infos = [lower_info(l) if not (u >> 31) else lower_info(0) for (l, u) in pairs]
    uinfo = [upper_info(u) for (l, u) in pairs]
    # structural checks
    for i, (l, u) in enumerate(pairs):
        br = is_branch(l, u)
        if br:
            if i != n - 2:
                return None            # branch must be the second to last pair (delay slot last)
            if (u >> 30) & 1 or (u >> 28) & 1 or (u >> 27) & 1:
                return None
            if is_branch(*pairs[n - 1]):
                return None            # branch in a delay slot
    last_is_branch_block = n >= 2 and is_branch(*pairs[n - 2])
    if n >= 1 and is_branch(*pairs[n - 1]) and not last_is_branch_block:
        return None                     # branch without delay slot
    # register file in locals unless a pair calls out to the exact engine (it reads and writes the VU1State)
    global RG, BND
    LAST.clear()
    calls_out = any((not (u >> 31) and infos[i]['calls_out']) or classify(u)[0] in ('opmsub', 'opmula', 'other')
                    for i, (l, u) in enumerate(pairs))
    if regs is not None:
        if calls_out:
            return None
        RG = regs
    else:
        inv = set()
        if selfname and LOCALS and not calls_out:
            # a block that may repeat itself: the VF registers it never writes get their operand view once (Regs.inv)
            pre = Regs(True)
            saved, BND = BND, None
            if gen_block(pc0, pairs, [], None, regs=pre, pieces={}) == 'pieces':
                inv = {k[1] for k in pre.defined if k[0] == 'f' and k not in pre.dirty and k[1] != 0}
            BND = saved
        RG = Regs(LOCALS and not calls_out)
        RG.inv = inv
    # Flag model of the exact engine: an FMAC / CLIP / FCSET issued at pair j becomes visible to flag readers 4 cycles later
    # (kFmacLatency, on the exact engine's stall-model cycles n.cyc, computed at block entry from the pipeline state the
    # previous block left). Flags older than the block come from the exact engine's
    # flag pipeline as visible at the reader's issue cycle (VU1NativeAccess::macAt / clipAt): writers of the previous block
    # (or of exact-stepped pairs) that are still in flight stay invisible until their latency has passed. At a block exit the
    # writers that can still become visible are queued in that pipeline (pushMac / pushClip) instead of being committed:
    # committing the newest one at once let a loop read the flags of its last pair too early (the shadow-volume culling loop
    # at 0x3C38 tests with FMEQ the sign of the MSUB issued exactly 4 pairs earlier, across the loop branch).
    FLAT = 4
    mac_w = [i for i in range(n) if uinfo[i][2]]
    clip_w = [i for i in range(n) if classify(pairs[i][1])[0] == 'clip' or (not (pairs[i][1] >> 31) and (pairs[i][0] >> 25) == 0x11)]
    for i in clip_w:
        if classify(pairs[i][1])[0] == 'clip' and not (pairs[i][1] >> 31) and (pairs[i][0] >> 25) == 0x11:
            return None

    def visible(i, writers, prefix, entry):
        """Newest earlier writer already committed when pair i issues: pair distance >= 4 always, nearer ones only when the
        exact engine's stall model (n.cyc) pushed pair i far enough."""
        expr = entry
        for j in [j for j in writers if j < i]:        # oldest -> newest; the newest candidate is tested first
            if i - j >= FLAT:
                expr = f"{prefix}{j}"
            else:
                expr = f"((n.cyc[{i}] - n.cyc[{j}] >= {FLAT}) ? {prefix}{j} : {expr})"
        return expr

    def mac_src(i):
        return visible(i, mac_w, "m", f"VU1NativeAccess::macAt(n, n.cyc[{i}])")

    def clip_src(i):
        return visible(i, clip_w, "c", f"VU1NativeAccess::clipAt(n, n.cyc[{i}])")

    def clip_prev(i):
        c = [j for j in clip_w if j < i]
        return f"c{c[-1]}" if c else "VU1NativeAccess::clipChain(n)"

    def flush_final(i):
        """Block end: the newest MAC / clip writer is queued with the block's tail record (n.tail); the older writers that
        can still be the visible one after the exit (successor in the last three pairs) are only stored there, the reader
        decides (VU1FlagRing::tailVisible)."""
        out, desc = [], {"m": [], "c": []}
        for writers, prefix in ((mac_w, "m"), (clip_w, "c")):
            w = [j for j in writers if j <= i]
            older = [j for k, j in enumerate(w[:-1]) if i - w[k + 1] < FLAT - 1]
            desc[prefix] = older
            for k, j in enumerate(older):
                out.append(f"n.tail->q[{k}] = q{j};" if prefix == "m" else f"n.tail->c[{k}] = c{j};")
        has_tail = bool(desc["m"] or desc["c"])
        tail = "n.tail" if has_tail else "nullptr"
        if mac_w and mac_w[-1] <= i:
            j = mac_w[-1]
            out.append(f"VU1NativeAccess::pushMacRaw(n, n.cyc[{j}], q{j}, {REV4[(pairs[j][1] >> 21) & 0xF]}u, {tail});")
        if clip_w and clip_w[-1] <= i:
            j = clip_w[-1]
            out.append(f"VU1NativeAccess::pushClipTail(n, n.cyc[{j}], c{j}, {tail});")
        if has_tail and tail_out is not None:
            tail_out.append((desc["m"], [REV4[(pairs[j][1] >> 21) & 0xF] for j in desc["m"]], desc["c"]))
        return out

    def flush(i):
        """Block exit after pair i (D/T halt): queue every writer that can still be the visible one at or after the exit
        (the last one, and older ones whose successor is not visible yet when the block ends)."""
        out = []
        end = f"n.cyc[{i}] + 1u"
        for writers, prefix, push in ((mac_w, "m", "pushMac"), (clip_w, "c", "pushClip")):
            w = [j for j in writers if j <= i]
            for k, j in enumerate(w):
                if prefix == "m":  # MAC flags of a writer read after the block: computed by the reader (VU1FlagRing)
                    call = f"VU1NativeAccess::pushMacRaw(n, n.cyc[{j}], q{j}, {REV4[(pairs[j][1] >> 21) & 0xF]}u, nullptr);"
                else:
                    call = f"VU1NativeAccess::{push}(n, n.cyc[{j}], {prefix}{j});"
                if k == len(w) - 1:
                    out.append(call)
                elif i - w[k + 1] < FLAT - 1:
                    # visible after the exit only if its successor is not yet: issue-cycle gaps are at least the pair
                    # distance (stalls only widen them), so writers followed by one 3+ pairs before the end never are
                    out.append(f"if (n.cyc[{w[k + 1]}] + {FLAT}u > {end}) {call}")
        return out

    lines = []
    branch_targets = []         # constant branch target of the block's branch (self-loops), none for JR / JALR
    # vf lanes known to hold finite values (their operand view is the raw value): {vf: SSE lane mask}. Set by the
    # results of this block that cannot be Inf/NaN (clamped FMAC results, MAX/MINI of normalised operands, ITOF, ABS),
    # cleared by any other write. The block starts (and a self-loop repeats) with nothing known.
    clean_regs = {}
    for i, (l, u) in enumerate(pairs):
        pc = pc0 + 8 * i
        clean_i = {r: lm for r, lm in clean_regs.items() if r not in infos[i]['vfw']}
        li, (uw, ur, umac, uq) = infos[i], uinfo[i]
        iBit = bool(u >> 31)
        lower_first = False
        both = False
        if not iBit:
            h1 = bool(uw & li['vfr'])            # upper writes what lower reads (lower must read old)
            h2 = bool(li['vfw'] & ur)            # lower writes what upper reads (upper must read old)
            h3 = li['writes_q'] and uq           # lower writes Q, upper reads old Q
            if (h1 and (h2 or h3)):
                return None
            lower_first = h1
        # backup of VI for a branch in the next pair
        backup = []
        if i + 1 < n and li['delay']:
            nl, nu = pairs[i + 1]
            if is_branch(nl, nu):
                nf = lower_fields(nl)
                regs = lower_info(nl)['vi_reads']
                if li['delay'] in regs:
                    backup.append(f"const int32_t vib{i + 1} = {RG.vi(li['delay'])};")
        lines.append(f"// pair {i} pc=0x{pc:04x} lower={l:08x} upper={u:08x}")
        lines.extend(backup)
        if is_branch(l, u):
            op = l >> 25
            f = lower_fields(l)
            ft, fs, imm = f['ft'], f['fs'], f['imm11']
            prev_backup = None
            def rd(reg):
                if i > 0 and infos[i - 1]['delay'] == reg and reg != 0:
                    return f"vib{i}"
                return RG.vi(reg)
            tgt = (pc + 8 + imm * 8) & 0x3FFF
            branch_targets = [] if op in (0x24, 0x25) else [tgt]
            if op == 0x20:
                lines.append("bool taken = true;")
                lines.append(f"uint32_t target = {tgt}u;")
            elif op == 0x21:
                if ft:
                    lines.append(RG.setvi(ft, f"{(pc + 16) // 8}"))
                lines.append("bool taken = true;")
                lines.append(f"uint32_t target = {tgt}u;")
            elif op == 0x24:
                lines.append("bool taken = true;")
                lines.append(f"uint32_t target = (((uint32_t)(uint16_t){RG.vi(fs)}) * 8u) & 0x3FFFu;")
            elif op == 0x25:
                lines.append(f"uint32_t target = (((uint32_t)(uint16_t){RG.vi(fs)}) * 8u) & 0x3FFFu;")
                if ft:
                    lines.append(RG.setvi(ft, f"{(pc + 16) // 8}"))
                lines.append("bool taken = true;")
            else:
                cmp = {0x28: "==", 0x29: "!="}.get(op)
                if cmp:
                    cond = f"(int16_t){rd(fs)} {cmp} (int16_t){rd(ft)}"
                else:
                    cmp = {0x2C: "< 0", 0x2D: "> 0", 0x2E: "<= 0", 0x2F: ">= 0"}[op]
                    cond = f"(int16_t){rd(fs)} {cmp}"
                lines.append(f"bool taken = {cond};")
                lines.append(f"uint32_t target = {tgt}u;")
            # upper still executes in the branch pair
            lines.extend(emit_upper(u, pc, i, f"m{i}" if i in mac_w else None, f"c{i}", clip_prev(i), clean_i))
            continue
        # ordinary pair
        # generated in execution order: the register file (Regs) loads a register at block entry when its first use reads it
        if lower_first:
            lo = emit_lower(l, pc, u, None, mac_src(i), clip_src(i), f"c{i}")
            up = emit_upper(u, pc, i, f"m{i}" if i in mac_w else None, f"c{i}", clip_prev(i), clean_i)
        else:
            up = emit_upper(u, pc, i, f"m{i}" if i in mac_w else None, f"c{i}", clip_prev(i), clean_i)
            lo = [] if iBit else emit_lower(l, pc, u, None, mac_src(i), clip_src(i), f"c{i}")
        if iBit:
            lines.extend(up)
            lines.append(f"s.i = vu1n::qnorm(*(const float *)&(const uint32_t &)(0x{l:08x}u)); // I register")
            # (the line above is replaced below to avoid UB)
            lines[-1] = f"{{ const uint32_t bits = 0x{l:08x}u; float imm; std::memcpy(&imm, &bits, 4); {RG.setireg('vu1n::qnorm(imm)')} }}"
            if BND is not None:
                BND.st[('I',)] = _f32_e(l)
        elif lower_first:
            lines.extend(lo)
            lines.extend(up)
        else:
            lines.extend(up)
            lines.extend(lo)
        clean_regs = dict(clean_i)
        kind_u, src_u, toacc_u, _ = classify(u)
        m_u = REV4[(u >> 21) & 0xF]
        finite = (kind_u in ('add', 'sub', 'mul', 'madd', 'msub') or kind_u in ('itof', 'abs')
                  or (kind_u in ('max', 'mini') and src_u in ('bc', 'v')))
        for x in uw:
            if finite and x not in li['vfw']:
                clean_regs[x] = clean_regs.get(x, 0) | m_u
            else:
                clean_regs.pop(x, None)
        # D/T halt bits
        if (u >> 28) & 1:
            lines.append("if (s.dBitEnabled) { " + " ".join(flush(i) + RG.writeback()) + f" s.stoppedByD = true; return kVu1nEnded | {(pc + 8) & 0x3FFF}u; }}")
        if (u >> 27) & 1:
            lines.append("if (s.tBitEnabled) { " + " ".join(flush(i) + RG.writeback()) + f" s.stoppedByT = true; return kVu1nEnded | {(pc + 8) & 0x3FFF}u; }}")
    end_pc = (pc0 + 8 * n) & 0x3FFF
    ebit = any((u >> 30) & 1 for (l, u) in pairs)
    lines.extend(flush_final(n - 1))
    if ebit:
        if pieces is not None:
            return None
        return RG.decls() + lines + RG.writeback() + [f"return kVu1nEnded | {end_pc}u;"]
    # exit: a branch back to the block's own start repeats it with the registers still in locals, when chainTo agrees
    nx = f"taken ? target : {end_pc}u" if last_is_branch_block else f"{end_pc}u"
    if pieces is not None:
        pieces.update(lines=lines, nx=nx, end_pc=end_pc)
        return 'pieces'
    self_loop = bool(selfname) and RG.local and (pc0 in branch_targets or (not last_is_branch_block and end_pc == pc0))
    out = RG.decls()
    LAST.update(self_loop=self_loop, decls=len(out), q_dirty=('q',) in RG.dirty, local=RG.local)
    if not self_loop:
        return out + lines + RG.writeback() + [f"return {nx};"]
    q_dirty = ('q',) in RG.dirty
    exit_lines = [f"const uint32_t nx = {nx};", "uint32_t lg = 0u;", f"if (nx == {pc0}u)", "{"]
    exit_lines += ["    s.q = QR; // FDIV timing (scalarStart) reads it"] if q_dirty else []
    exit_lines += [f"    lg = VU1NativeAccess::chainTo(n, {pc0}u, {selfname});"]
    exit_lines += ["    QR = s.q;"] if q_dirty else []
    exit_lines += ["    if (lg == 1u)", "        continue;", "}"]
    exit_lines += RG.writeback() + ["return nx | (lg ? kVu1nFinished : 0u);"]
    return out + ["for (;;)", "{"] + ["    " + ln for ln in lines + exit_lines] + ["}"]


def branch_target(pc0, pairs):
    """static taken target of a block ending in B / BAL / a conditional branch (None for JR / JALR or no branch)"""
    n = len(pairs)
    if n < 2 or not is_branch(*pairs[n - 2]):
        return None
    l, _ = pairs[n - 2]
    op = l >> 25
    if op in (0x24, 0x25):
        return None
    return (pc0 + 8 * (n - 2) + 8 + sx(l & 0x7FF, 11) * 8) & 0x3FFF


def gen_cycle(pc_a, pairs_a, pc_b, pairs_b, name_a, name_b, bstate=None):
    """Two blocks that branch to each other (a loop unrolled into two blocks, e.g. the vertex loop at 0x2840/0x2898 of
    the main renderer, docs/vu1-hle.md): one function for A that goes on into B and back while chainTo agrees, the
    registers of both in locals the whole time. Before, every pass through the loop left through runNative twice (each
    block loading and storing its ~20 registers). Exits: like the single blocks (writeback, next pc). Returns the body
    lines or None (falls back to A's own function)."""
    global BND
    if not (LOCALS and LOOPS):
        return None
    saved, BND = BND, None
    # pass 1: the registers either block touches; pass 2: generated with all of them loaded at entry and stored at every
    # exit (an exit after A has to store what B wrote in earlier passes)
    union = Regs(True)
    for pc, pairs in ((pc_a, pairs_a), (pc_b, pairs_b)):
        p = {}
        if gen_block(pc, pairs, [], None, regs=union, pieces=p) != 'pieces':
            BND = saved
            return None
    BND = saved
    seeded = Regs(True)
    seeded.defined = list(union.defined)
    seeded.need = list(union.defined)
    seeded.dirty = list(union.dirty)
    seeded.inv = {k[1] for k in union.defined if k[0] == 'f' and k not in union.dirty and k[1] != 0}
    pa, pb = {}, {}
    # fast variant (bstate: A's start state, B's start state without A's end): A, then B from A's end joined
    if bstate is not None:
        BND = Bounds(bstate['SA'])
    if gen_block(pc_a, pairs_a, [], None, regs=seeded, pieces=pa) != 'pieces':
        BND = saved
        return None
    info = {}
    if bstate is not None:
        info['end_A'] = BND.st
        sb = _q_unknown(BND.st)
        if bstate.get('Bextra') is not None:
            sb = Bounds.join_states(sb, bstate['Bextra'])
        nb = Bounds(sb)
        nb.req, nb.score, nb.fails = BND.req, BND.score, BND.fails
        info['SB'] = nb.st
        BND = nb
    if gen_block(pc_b, pairs_b, [], None, regs=seeded, pieces=pb) != 'pieces':
        BND = saved
        return None
    if bstate is not None:
        info.update(end_B=BND.st, req=BND.req, score=BND.score, fails=BND.fails)
    BND = saved
    q_dirty = ('q',) in seeded.dirty
    wb = seeded.writeback()

    def chain(pc, fn, then):
        out = ["    s.q = QR; // FDIV timing (scalarStart) reads it"] if q_dirty else []
        out += [f"    lg = VU1NativeAccess::chainTo(n, {pc}u, {fn});"]
        out += ["    QR = s.q;"] if q_dirty else []
        out += ["    if (lg == 1u)", f"        {then};"]
        return out

    def exit_of(p, own_pc, own_fn, own_then, other_pc, other_fn, other_then):
        out = [f"const uint32_t nx = {p['nx']};", "uint32_t lg = 0u;"]
        out += [f"if (nx == {other_pc}u)", "{"] + chain(other_pc, other_fn, other_then) + ["}"]
        out += [f"else if (nx == {own_pc}u)", "{"] + chain(own_pc, own_fn, own_then) + ["}"]
        return out + wb + ["return nx | (lg ? kVu1nFinished : 0u);"]

    body = seeded.decls()
    info['decls'] = len(body)
    LAST.clear()
    LAST.update(info)
    body += ["for (;;)", "{", "    {"]
    body += ["        " + ln for ln in pa['lines'] + exit_of(pa, pc_a, name_a, "continue", pc_b, name_b, "goto cycleB")]
    body += ["    }", "cycleB:", "    {"]
    body += ["        " + ln for ln in pb['lines'] + exit_of(pb, pc_b, name_b, "goto cycleB", pc_a, name_a, "continue")]
    body += ["    }", "}"]
    return body


def static_succs(pc, pairs):
    """static successors of a block (taken target and fall-through of a conditional branch, the target of B / BAL, the
    next pc of a block without a branch), None for JR / JALR or an E-bit block"""
    n = len(pairs)
    end_pc = (pc + 8 * n) & 0x3FFF
    if any((u >> 30) & 1 for (l, u) in pairs):
        return None
    if n >= 2 and is_branch(*pairs[n - 2]):
        op = pairs[n - 2][0] >> 25
        if op in (0x24, 0x25):
            return None
        tgt = branch_target(pc, pairs)
        return [tgt] if op in (0x20, 0x21) else list(dict.fromkeys([tgt, end_pc]))
    return [end_pc]


_LOCAL_OK = {}      # loop_regions: block key -> the block can run with its registers in locals (gen_block pieces)
_REGION_MEMO = {}   # loop_regions: a component's graph (blocks, edges, entries) -> its loops; images share most code


def loop_regions(code, heads):
    """loops of the image's static block graph (strongly connected components, or those inside a larger one) with two or
    more blocks within REGION_MAX_BLOCKS / REGION_MAX_PAIRS (one block: self-loops; two-block cycles that gen_cycle took
    keep it): [(member pcs, the members whose function runs this loop)], each block in the loop it leaves least often"""
    global BND
    ext, succ, full = {}, {}, {}
    for pc in heads:
        pairs = extent(code, pc)
        if not pairs:
            continue
        ext[pc] = pairs
        full[pc] = static_succs(pc, pairs) or [None]
    # blocks that cannot keep their registers in locals (they call out to the exact engine) end every loop
    saved, BND = BND, None
    inner = set()
    for pc in ext:
        k = fnv_block(pc, [w for pr in ext[pc] for w in pr])
        if k not in _LOCAL_OK:
            _LOCAL_OK[k] = gen_block(pc, ext[pc], [], None, regs=Regs(True), pieces={}) == 'pieces'
        if _LOCAL_OK[k]:
            inner.add(pc)
    BND = saved
    for pc in inner:
        succ[pc] = [t for t in full[pc] if t in inner]

    def sccs(nodes):
        reach = {}
        for v in nodes:
            seen, todo = set(), [v]
            while todo:
                x = todo.pop()
                for w in succ.get(x, ()):
                    if w in nodes and w not in seen:
                        seen.add(w)
                        todo.append(w)
            reach[v] = seen
        comps, done = [], set()
        for v in sorted(nodes):
            if v in done or v not in reach[v]:
                continue
            comp = sorted(w for w in reach[v] if v in reach[w])
            done.update(comp)
            comps.append(comp)
        return comps

    def fits(comp):
        return len(comp) <= REGION_MAX_BLOCKS and sum(len(ext[q]) for q in comp) <= REGION_MAX_PAIRS

    def decompose(top):
        # candidates: the components that fit, else those of the component without one of its blocks (every choice:
        # the outer loop's header is not always an entry block, as blocks overlap), without its entry blocks when large
        cands, seen = set(), set()
        todo = [top]
        while todo and len(seen) < 2000:
            comp = todo.pop()
            key = frozenset(comp)
            if key in seen:
                continue
            seen.add(key)
            if fits(comp):
                cands.add(key)
                continue
            cs = set(comp)
            if len(comp) > 2 * REGION_MAX_BLOCKS:
                entries = {w for v in succ if v not in cs for w in succ[v] if w in cs} or {comp[0]}
                todo += [c for c in sccs(cs - entries) if len(c) >= 2]
                continue
            for v in comp:
                todo += [c for c in sccs(cs - {v}) if len(c) >= 2]

        # every block: the candidate that a pass leaves least often (fewest exit edges per edge inside: an inner loop
        # rather than the outer one around it), then the one with most pairs
        def rank(c):
            inside = sum(1 for v in c for w in succ[v] if w in c)
            exits = sum(1 for v in c for w in full[v] if w not in c)
            return (exits / inside, -sum(len(ext[q]) for q in c), -len(c), sorted(c))
        best = {}
        for c in sorted(cands, key=rank):
            for v in c:
                best.setdefault(v, c)
        out = {}
        for v, c in best.items():
            out.setdefault(c, []).append(v)
        return [(sorted(c), sorted(roots)) for c, roots in out.items()]

    result = []
    for top in sccs(set(succ)):
        if len(top) < 2:
            continue
        ts = set(top)
        entries = sorted({w for v in succ if v not in ts for w in succ[v] if w in ts})
        sig = (tuple((v, fnv_block(v, [w for pr in ext[v] for w in pr]), tuple(full[v])) for v in top), tuple(entries))
        if sig not in _REGION_MEMO:
            _REGION_MEMO[sig] = decompose(top)
        result += _REGION_MEMO[sig]
    return result


_EXIT_RE = re.compile(r'^(\s*)return ((?:nx|taken \?|\d+u).*);$')


def gen_region(root, members, code, names, bodies):
    """A loop of two or more blocks (e.g. the shadow-volume loop at 0x0cd8-0x0e10, the PC zones' loop at 0x1728-0x1bc8):
    one function entered at `root` that runs the loop's blocks one after the other while chainTo agrees, each block with
    its own function body (fast variant and entry check, self-loop or cycle included) in its own scope, so it keeps only
    its own registers in locals (a first version kept the registers of all the blocks in locals across the loop: GCC
    spilled them and it was slower). It saves runNative's dispatch per block, not the register loads and stores. bodies:
    pc -> the body lines of that block's own function; its exits to the next pc (`return nx...`) go to the chain step."""
    order = [root] + [p for p in members if p != root]
    out = []
    for p in order:
        lines = []
        for ln in bodies[p]:
            ln = ln.replace("cycleB", f"cycleB_{p:04x}")   # gen_cycle's label, once per scope
            m = _EXIT_RE.match(ln)
            lines.append(f"{m.group(1)}{{ rx = {m.group(2)}; goto exit_{p:04x}; }}" if m else ln)
        step = ["if (!(rx & (kVu1nEnded | kVu1nBail | kVu1nFinished)))", "{", "    uint32_t lg = 0u;"]
        kw = "if"
        for t in static_succs(p, extent(code, p)) or []:
            if t in names:
                step += [f"    {kw} (rx == {t}u)", "    {", f"        lg = VU1NativeAccess::chainTo(n, {t}u, {names[t]});",
                         "        if (lg == 1u)", f"            goto region_{t:04x};", "    }"]
                kw = "else if"
        step += ["    return rx | (lg ? kVu1nFinished : 0u);", "}", "return rx;"]
        # the barrier: the next block reloads its registers from the VU1State (else GCC forwards the stores across the
        # chain step and keeps every block's registers live through the whole function, spilling them)
        out += [f"region_{p:04x}:", "{", "    VU1N_REGION_BARRIER();", "    uint32_t rx;", "    {"] + ["        " + ln for ln in lines]
        out += ["    }", f"exit_{p:04x}:"] + ["    " + ln for ln in step] + ["}"]
    return out


def _q_unknown(st):
    st = dict(st)
    st[('q',)] = None   # Q is reloaded from the state after chainTo (commitDue may write it)
    return st


def _lim(E):
    """largest magnitude bits below 2^E; E = 128: the largest finite value (Inf / NaN fail)"""
    return ((min(E, 128) + 127) << 23) - 1


def _check_lines(req, Emap, Edef, xname):
    """entry check of a fast variant: every entry lane its code relied on (origin) below 2^E in magnitude (E per origin),
    else the exact function"""
    vf, acc, scal = {}, {}, []
    for o, level in req.items():
        E = Emap.get(o, Edef) if level == 2 else 128
        if o[0] == 'f':
            vf.setdefault(o[1], {})[o[2]] = E
        elif o[0] == 'acc':
            acc[o[1]] = E
        else:
            scal.append(({'q': 'QR', 'I': 'IR'}[o[0]], E))

    def limv(lanes):
        l = [_lim(lanes[k]) if k in lanes else 0x7FFFFFFF for k in range(4)]
        return f"_mm_set_epi32(0x{l[3]:08x}, 0x{l[2]:08x}, 0x{l[1]:08x}, 0x{l[0]:08x})"
    pairs = [(f"F{r}", limv(ln)) for r, ln in sorted(vf.items())]
    if acc:
        pairs.append(("ACC", limv(acc)))
    pairs += [(f"_mm_set_ss({x})", limv({0: E})) for x, E in sorted(scal)]
    if not pairs:
        return []
    terms = [f"vu1nf::gt({v}, {lv})" for v, lv in pairs]
    # registers with the same per-lane limits: maximum of the magnitudes first (two vector ops per register), one compare
    groups = {}
    for v, lv in pairs:
        groups.setdefault(lv, []).append(f"vu1nf::mg({v})")
    cmps = []
    for lv, ms in groups.items():
        m = ms[0]
        for x in ms[1:]:
            m = f"_mm_max_epu32({m}, {x})"
        cmps.append(f"_mm_cmpgt_epi32({m}, {lv})")
    expr = cmps[0]
    for t in cmps[1:]:
        expr = f"_mm_or_si128({expr}, {t})"
    out = [f"if (vu1nf::any({expr})) // fast variant: entry values in range (vu1n_gen.py Bounds)",
           "{",
           f"    VU1NF_MISS(\"{xname}\");",
           "#if defined(VU1N_FAST_STATS_REGS)"]
    out += [f"    if (vu1nf::any({t})) vu1nf::why(\"{xname}\", \"{v[:12]}\", {v}, {lv});" for t, (v, lv) in zip(terms, pairs)]
    out += ["#endif", f"    return {xname}(n);", "}", "VU1NF_HIT();"]
    return out


def _fixpoint(gen, entry):
    """gen(start) -> list of end-of-pass states that flow back into the start. Iterates the start state of a loop function
    to its fixed point (join with the entry state, widening after a few rounds); returns the final start state"""
    start = entry
    for it in range(40):
        back = gen(start)
        if back is None:
            return None
        new = entry
        for b in back:
            new = Bounds.join_states(new, _q_unknown(b))
        if it >= 6:
            new = Bounds.widen(start, new)
        if new == start:
            return start
        start = new
    return None


FAST_E0 = 64      # entry bound tried first for every origin
FAST_FLOOR = 16   # lowest entry bound the search goes to
FAST_STEP = 4


def _search(run):
    """run(Emap) -> (body, decls, score, req, fails) or None. Entry bounds per origin: all start at FAST_E0; while MADDs
    stay on maddc although every lane is known, the origins they depend on that have the highest bound are lowered by
    FAST_STEP (balanced: a chain of matrix transforms ends with equal bounds), keeping the best-scoring map."""
    Emap = {}
    cur = run(Emap)
    if cur is None:
        return None
    best = (cur, dict(Emap))
    stall = 0
    for step in range(80):
        cand = {}
        for origs in cur[4]:
            for o in origs:
                if Emap.get(o, FAST_E0) > FAST_FLOOR:
                    cand[o] = Emap.get(o, FAST_E0)
        if not cand:
            break
        top = max(cand.values())
        for o, e in cand.items():
            if e == top:
                Emap[o] = e - FAST_STEP
        cur = run(Emap)
        if cur is None:
            break
        if cur[2] > best[0][2]:
            best = (cur, dict(Emap))
            stall = 0
        else:
            stall += 1
            if stall >= 14:
                break
    return best


def fast_self_loop(pc, pairs, name):
    """fast variant of a self-loop block: (body lines, Emap, score) or None"""
    def run(Emap):
        global BND
        entry = Bounds.entry(Emap, FAST_E0)

        def gen(start):
            global BND
            BND = Bounds(start)
            body = gen_block(pc, pairs, [], name)
            end = BND.st
            BND = None
            return None if body is None else [end]
        start = _fixpoint(gen, entry)
        if start is None:
            return None
        BND = Bounds(start)
        body = gen_block(pc, pairs, [], name)
        b, BND = BND, None
        if body is None or not LAST.get('self_loop'):
            return None
        return (body, LAST['decls'], b.score, b.req, b.fails)
    best = _search(run)
    if best is None or not best[0][2]:
        return None
    (body, nd, score, req, _), Emap = best
    return body[:nd] + _check_lines(req, Emap, FAST_E0, name + "_x") + body[nd:], Emap, score


def _check_cost(req):
    """instructions of the entry check: two per register (magnitude, maximum), a compare per limit vector, the branch"""
    regs = {o[:2] for o in req}
    return 2 * len(regs) + 6


FAST_STRAIGHT_MIN = 8   # a block that runs once per entry needs this many instructions saved beyond its check


def fast_straight(pc, pairs, name):
    """fast variant of a block without a loop: one pass, no fixed point; only when it saves more than its check costs"""
    def run(Emap):
        global BND
        BND = Bounds(Bounds.entry(Emap, FAST_E0))
        body = gen_block(pc, pairs, [], name)
        b, BND = BND, None
        if body is None or LAST.get('self_loop') or 'decls' not in LAST:
            return None
        return (body, LAST['decls'], b.score, b.req, b.fails)
    best = _search(run)
    if best is None or best[0][2] - _check_cost(best[0][3]) < FAST_STRAIGHT_MIN:
        return None
    (body, nd, score, req, _), Emap = best
    return body[:nd] + _check_lines(req, Emap, FAST_E0, name + "_x") + body[nd:], Emap, score


def fast_cycle(pc, pairs, tgt, pairs_b, name, name_b):
    def run(Emap):
        entry = Bounds.entry(Emap, FAST_E0)
        sa, bextra = entry, None
        for it in range(40):
            if gen_cycle(pc, pairs, tgt, pairs_b, name, name_b, {'SA': sa, 'Bextra': bextra}) is None:
                return None
            ea, eb = _q_unknown(LAST['end_A']), _q_unknown(LAST['end_B'])
            nsa = Bounds.join_states(Bounds.join_states(entry, ea), eb)
            nbx = eb
            if it >= 6:
                nsa = Bounds.widen(sa, nsa)
                nbx = Bounds.widen(bextra, nbx) if bextra is not None else nbx
            # B's start = join(A's end, B's end): stable when A's start and B's own back edge are
            if nsa == sa and nbx == bextra:
                body = gen_cycle(pc, pairs, tgt, pairs_b, name, name_b, {'SA': sa, 'Bextra': bextra})
                if body is None:
                    return None
                return (body, LAST['decls'], LAST['score'], LAST['req'], LAST['fails'])
            sa, bextra = nsa, nbx
        return None
    best = _search(run)
    if best is None or not best[0][2]:
        return None
    (body, nd, score, req, _), Emap = best
    return body[:nd] + _check_lines(req, Emap, FAST_E0, name + "_x") + body[nd:], Emap, score


REGION_PREAMBLE = r"""// loop regions (gen_region): registers go through the VU1State between the blocks
#if defined(_MSC_VER)
#define VU1N_REGION_BARRIER() _ReadWriteBarrier()
#else
#define VU1N_REGION_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif
"""

FAST_PREAMBLE = r"""// fast variants (vu1n_gen.py Bounds): entry check helpers, FTOI of a finite view
#if defined(VU1N_FAST_STATS) // entry checks passed / failed, reported at exit (replay harness)
#include <cstdio>
namespace vu1nf
{
    inline uint64_t g_hit = 0, g_miss = 0;
    struct Report
    {
        ~Report() { std::fprintf(stderr, "[vu1nf] fast %llu, exact %llu\n", (unsigned long long)g_hit, (unsigned long long)g_miss); }
    };
    inline Report g_report;
}
namespace vu1nf
{
    inline void why(const char *fn, const char *what, __m128 v, __m128i lim)
    {
        static int n = 0;
        alignas(16) uint32_t b[4], l[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(b), _mm_castps_si128(v));
        _mm_store_si128(reinterpret_cast<__m128i *>(l), lim);
        if (n++ < 400)
            std::fprintf(stderr, "[vu1nf] why %s %s %08x/%08x %08x/%08x %08x/%08x %08x/%08x\n", fn, what, b[0], l[0], b[1], l[1], b[2], l[2], b[3], l[3]);
    }
}
#define VU1NF_HIT() (++vu1nf::g_hit)
#define VU1NF_MISS(name) do { static int c_ = 0; ++vu1nf::g_miss; if (c_++ < 2) std::fprintf(stderr, "[vu1nf] miss %s\n", name); } while (0)
#else
#define VU1NF_HIT() ((void)0)
#define VU1NF_MISS(name) ((void)0)
#endif
namespace vu1nf
{
    // magnitude bits
    VU1N_INLINE __m128i mg(__m128 v) { return _mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF)); }
    // lanes whose magnitude bits exceed the per-lane limit (Inf / NaN exceed every limit below 0x7F800000)
    VU1N_INLINE __m128i gt(__m128 v, __m128i lim)
    {
        return _mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF)), lim);
    }
    VU1N_INLINE bool any(__m128i m)
    {
#if defined(__aarch64__)
        return vmaxvq_u32(vreinterpretq_u32_s32(vreinterpretq_s32_m128i(m))) != 0u;
#else
        return _mm_movemask_epi8(m) != 0;
#endif
    }
    // vu1n::clipValv in vector compares (same compares, same flush-to-zero mode)
    VU1N_INLINE uint32_t clipv(__m128 v, __m128 t, uint32_t prev)
    {
        const __m128 w = _mm_and_ps(_mm_shuffle_ps(t, t, _MM_SHUFFLE(3, 3, 3, 3)), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
        const __m128 nw = _mm_xor_ps(w, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))));
        const __m128i gt = _mm_castps_si128(_mm_cmpgt_ps(v, w));
        const __m128i lt = _mm_castps_si128(_mm_cmplt_ps(v, nw));
#if defined(__aarch64__)
        const uint32x4_t bits = vorrq_u32(vandq_u32(vreinterpretq_u32_m128i(gt), (uint32x4_t){1u, 4u, 16u, 0u}),
                                          vandq_u32(vreinterpretq_u32_m128i(lt), (uint32x4_t){2u, 8u, 32u, 0u}));
        const uint32_t f = vaddvq_u32(bits);
#else
        const uint32_t g = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(gt))), l = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(lt)));
        const uint32_t f = (g & 1u) | ((l & 1u) << 1) | ((g & 2u) << 1) | ((l & 2u) << 2) | ((g & 4u) << 2) | ((l & 4u) << 3);
#endif
        return ((prev << 6) | f) & 0xFFFFFFu;
    }
    // vu1n::ftoiv without the clamp: the operand is a view (finite), where clampv is the identity
    template <int Shift>
    VU1N_INLINE __m128 ftoiv(__m128 finite)
    {
        constexpr float scale = Shift == 0 ? 1.0f : Shift == 4 ? 16.0f : Shift == 12 ? 4096.0f : 32768.0f;
        const __m128 v = _mm_mul_ps(finite, _mm_set1_ps(scale));
#if defined(__aarch64__)
        return vreinterpretq_f32_s32(vcvtq_s32_f32(v));
#else
        const __m128 big = _mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f));
        const __m128i i = _mm_cvttps_epi32(v);
        return _mm_blendv_ps(_mm_castsi128_ps(i), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), big);
#endif
    }
}

"""


def main():
    args = sys.argv[1:]
    heads_file = None
    parts = 6
    files = []
    out = args[0]
    i = 1
    while i < len(args):
        if args[i] == '--heads':
            heads_file = args[i + 1]
            i += 2
        elif args[i] == '--locals':
            global LOCALS
            LOCALS = True
            i += 1
        elif args[i] == '--no-fast':
            global FAST
            FAST = False
            i += 1
        elif args[i] == '--no-cycles':
            global CYCLES
            CYCLES = False
            i += 1
        elif args[i] == '--regions':
            global REGIONS
            REGIONS = True
            i += 1
        elif args[i] == '--parts':
            parts = int(args[i + 1])
            i += 2
        else:
            files.append(args[i])
            i += 1
    inputs = []
    for a in files:
        if os.path.isdir(a):
            inputs += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith('.bin'))
        else:
            inputs.append(a)
    extra = set()
    if heads_file and os.path.exists(heads_file):
        for line in open(heads_file):
            line = line.strip()
            if line:
                extra.add(int(line, 16) & 0x3FF8)
    blocks = {}
    unsupported = 0
    fused = {}            # A's key -> the other functions its function chains to (gen_cycle: B, gen_region: the loop)
    fast = {}             # key -> (fast variant body, E, score): the block's function; the exact one becomes <name>_x
    extern_names = set()  # B functions called by name from another block's function: not static, declared before use
    nregions = 0
    region_of = {}        # key -> the body of its loop-region function (gen_region)
    for f in inputs:
        code = open(f, 'rb').read()
        if len(code) != CODE_SIZE:
            continue
        for pc in sorted(heads_of(code, extra)):
            pairs = extent(code, pc)
            if not pairs:
                continue
            words = [w for pr in pairs for w in pr]
            key = fnv_block(pc, words)
            if key in blocks:
                continue
            name = f"b{pc:04x}_{key & 0xFFFFFFFF:08x}"
            tail = []
            body = gen_block(pc, pairs, tail, name if LOOPS and LOCALS else None)
            # bit 16 of the registration: the block itself reads flags written before it (decided on its own code, also
            # when gen_cycle replaces the body below)
            reads_entry = body is not None and any("macAt(" in ln or "clipAt(" in ln for ln in body)
            blocks[key] = (pc, len(pairs), body, tail[0] if tail else None, reads_entry)
            if body is None:
                unsupported += 1
            elif FAST and LAST.get('local') and 'decls' in LAST:
                fb = fast_self_loop(pc, pairs, name) if LAST.get('self_loop') else fast_straight(pc, pairs, name)
                if fb is not None:
                    fast[key] = fb
        # two-block cycles (A branches to B, B back to A): A's function runs both (gen_cycle)
        if CYCLES:
            for pc in sorted(heads_of(code, extra)):
                pairs = extent(code, pc)
                tgt = branch_target(pc, pairs) if pairs else None
                if tgt is None or tgt == pc:
                    continue
                pairs_b = extent(code, tgt)
                if not pairs_b or branch_target(tgt, pairs_b) != pc:
                    continue
                key = fnv_block(pc, [w for pr in pairs for w in pr])
                key_b = fnv_block(tgt, [w for pr in pairs_b for w in pr])
                if key in fused or key not in blocks or key_b not in blocks or blocks[key][2] is None or blocks[key_b][2] is None:
                    continue
                name = f"b{pc:04x}_{key & 0xFFFFFFFF:08x}"
                name_b = f"b{tgt:04x}_{key_b & 0xFFFFFFFF:08x}"
                body = gen_cycle(pc, pairs, tgt, pairs_b, name, name_b)
                if body is None:
                    continue
                fast.pop(key, None)
                if FAST:
                    fb = fast_cycle(pc, pairs, tgt, pairs_b, name, name_b)
                    if fb is not None:
                        fast[key] = fb
                fused[key] = [name_b]
                extern_names.add(name_b)
                p, npairs, _, tail, reads_entry = blocks[key]
                blocks[key] = (p, npairs, body, tail, reads_entry)
    # loops of more blocks (after every image's blocks and cycles): the loop's blocks get a function that runs the loop
    # from them (gen_region); the block's own function body stays as <name>_x when it has no exact twin yet
    if CYCLES and REGIONS:
        own = {key: (fast[key][0] if key in fast else blocks[key][2]) for key in blocks if blocks[key][2] is not None}
        for f in inputs:
            code = open(f, 'rb').read()
            if len(code) != CODE_SIZE:
                continue
            for comp, roots in loop_regions(code, heads_of(code, extra)):
                keys = {p: fnv_block(p, [w for pr in extent(code, p) for w in pr]) for p in comp}
                if any(k not in own for k in keys.values()):
                    continue
                names = {p: f"b{p:04x}_{keys[p] & 0xFFFFFFFF:08x}" for p in comp}
                for root in roots:
                    key = keys[root]
                    if key in fused or key in region_of:
                        continue
                    region_of[key] = gen_region(root, comp, code, names, {p: own[keys[p]] for p in comp})
                    fused[key] = [names[p] for p in comp if p != root]
                    for p in comp:
                        if keys[p] in fast:   # its fast body calls <name>_x from the region function
                            fused[key].append(names[p] + "_x")
                            extern_names.add(names[p] + "_x")
                    extern_names.update(names.values())
                    nregions += 1
    os.makedirs(out, exist_ok=True)
    texts = [[] for _ in range(parts)]
    regs = [[] for _ in range(parts)]
    count = 0
    for key, (pc, npairs, body, tail, reads_entry) in sorted(blocks.items()):
        if body is None:
            continue
        name = f"b{pc:04x}_{key & 0xFFFFFFFF:08x}"
        part = count % parts
        count += 1
        t = []
        for other in fused.get(key, []):
            t.append(f"{'VU1N_NOINLINE ' if other.endswith('_x') else ''}uint32_t {other}(VU1NativeCtx &n);")
        if tail:
            mp, mm, cp = (list(x) + [0] * (3 - len(x)) for x in tail)
            t.append(f"static const VU1TailDesc {name}_tail = {{{len(tail[0])}, {len(tail[2])}, {{{mp[0]}, {mp[1]}, {mp[2]}}}, "
                     f"{{{mm[0]}, {mm[1]}, {mm[2]}}}, {{{cp[0]}, {cp[1]}, {cp[2]}}}}};")
        if key in region_of:
            # the region function's first scope is the block's own (fast) body: its exact twin <name>_x stays
            t.append(f"{'' if name in extern_names else 'static '}uint32_t {name}(VU1NativeCtx &n);")
            if key in fast:
                t += [f"{'' if name + '_x' in extern_names else 'static '}VU1N_NOINLINE uint32_t {name}_x(VU1NativeCtx &n)", "{", "    VU1State &s = *n.s;"]
                t += ["    " + ln for ln in body]
                t += ["}", ""]
            body = region_of[key]
        elif key in fast:
            t.append(f"{'' if name in extern_names else 'static '}uint32_t {name}(VU1NativeCtx &n);")
            t += [f"{'' if name + '_x' in extern_names else 'static '}VU1N_NOINLINE uint32_t {name}_x(VU1NativeCtx &n)", "{", "    VU1State &s = *n.s;"]
            t += ["    " + ln for ln in body]
            t += ["}", ""]
            body = fast[key][0]
        t += [f"{'' if name in extern_names else 'static '}uint32_t {name}(VU1NativeCtx &n)", "{", "    VU1State &s = *n.s;"]
        t += ["    " + ln for ln in body]
        t += ["}", ""]
        texts[part].append("\n".join(t))
        # bit 16: the block reads flags written before it; only such blocks need exact entry timing (see runNative)
        regs[part].append(f"    vu1nRegisterBlock(0x{key:016x}ull, {name}, {npairs | (0x10000 if reads_entry else 0)}u, "
                          f"{'&' + name + '_tail' if tail else 'nullptr'});")
    header = ('// Generated by tools/vu1/vu1n_gen.py from the game\'s VU1 microcode. Do not publish.\n'
              '#include "runtime/ps2_vu1_native.h"\n#include <cmath>\n\n'
              '#if defined(_MSC_VER)\n#pragma float_control(precise, on, push)\n#pragma fp_contract(off)\n#endif\n\n'
              '#ifndef VU1N_HAS_MADDC // runtimes before agent/sw-vu1-hle: same result, clamp always\n'
              'namespace vu1n\n{\n    template <bool Sub>\n'
              '    inline __m128 maddc(__m128 acc, __m128 a, __m128 b, __m128 &raw)\n    {\n'
              '        raw = madd<Sub>(acc, a, b);\n        return clampv(raw);\n    }\n}\n#endif\n\n' + (REGION_PREAMBLE if REGIONS else '') + FAST_PREAMBLE)
    footer = '\n#if defined(_MSC_VER)\n#pragma float_control(pop)\n#endif\n'
    for k in range(parts):
        body = header + "\n".join(texts[k]) + "\nnamespace\n{\n    const bool g_registered = []\n    {\n" + "\n".join(regs[k]) + "\n        return true;\n    }();\n}\n" + footer
        path = os.path.join(out, f"vu1n_part{k}.cpp")
        old = open(path, encoding='utf-8').read() if os.path.exists(path) else None
        if old != body:
            open(path, 'w', encoding='utf-8').write(body)
    es = {}
    for v in fast.values():
        for e in v[1].values():
            es[e] = es.get(e, 0) + 1
    print(f"[vu1n_gen] {len(inputs)} images, {len(blocks)} distinct blocks, {count} generated, {unsupported} unsupported, "
          f"{len(fast)} fast variants (lowered entry bounds: {dict(sorted(es.items()))}), {nregions} loop-region functions")


if __name__ == '__main__':
    main()
