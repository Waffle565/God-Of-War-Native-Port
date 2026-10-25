"""deko3d shaders of the Switch GS renderer (docs/switch.md "deko3d renderer").

deko3d has no GLSL compiler at run time: every program the GL backend (ext gs_hw_backend.cpp) builds is compiled here
ahead of time with devkitPro's uam (GLSL -> DKSH, run in WSL) and embedded in the runner as a generated C++ file.
The Switch's GL emulation over deko3d (gs_dk.cpp) finds a shader by a hash of the exact source text the backend passes
to glShaderSource, so the backend keeps building its programs as on the PC.

uam has no loose (default-block) uniforms, so each program family's sources are rewritten:
  - one std140 block `GSU` (binding 0, same buffer in every stage) holds every non-opaque uniform of the family, in a
    fixed order, so all variants of a family share one layout; each loose `uniform T uX;` line becomes
    `#define uX (gsu_uX)` (a variant's own `#define uX 1` skips that line through the backend's #ifndef guards);
  - samplers get `layout(binding = N)`, numbered per family; glUniform1i(sampler, unit) maps binding N to a unit.
The generated file lists per shader: hash, stage, DKSH bytes, and the family's uniform table (name, std140 offset,
type, count) and sampler table (name, binding).

usage: python -I tools/dk_shaders.py <ext worktree> <variant keys file|-> <out .cpp> [--work <dir>]
  variant keys: hex draw-variant keys (one per line or "key=xxxxxx" anywhere in a log); the uber program is always in.
"""
import hashlib
import os
import re
import subprocess
import sys

WSL_UAM = '/opt/devkitpro/tools/bin/uam'

TYPES = {  # glsl type -> (std140 size, alignment, components, base)
    'float': (4, 4, 1, 'f'), 'vec2': (8, 8, 2, 'f'), 'vec3': (12, 16, 3, 'f'), 'vec4': (16, 16, 4, 'f'),
    'int': (4, 4, 1, 'i'), 'ivec2': (8, 8, 2, 'i'), 'ivec3': (12, 16, 3, 'i'), 'ivec4': (16, 16, 4, 'i'),
    'uint': (4, 4, 1, 'u'), 'uvec2': (8, 8, 2, 'u'), 'uvec3': (12, 16, 3, 'u'), 'uvec4': (16, 16, 4, 'u'),
}
OPAQUE = ('sampler2D', 'usampler2D', 'isampler2D')
UNIFORM_LINE = re.compile(r'^(\s*)(layout\s*\(([^)]*)\)\s*)?uniform\s+(\w+)\s+([\w\s,\[\]]+?)\s*;(.*)$')


def raw_strings(path):
    """{name: text} of every `const char *kName = R"(...)";` in a C++ file (exact text)."""
    src = open(path, encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'const\s+char\s*\*\s*(?:const\s+)?(k\w+)\s*=\s*R"\(', src):
        start = m.end()
        out[m.group(1)] = src[start:src.index(')"', start)]
    return out


def fnv64(text):
    h = 0xcbf29ce484222325
    for b in text.encode('utf-8'):
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


class Family:
    """uniforms and samplers of a set of programs sharing one GSU layout"""

    def __init__(self, name, sources):
        self.name = name
        self.uniforms = []  # (name, type)
        self.samplers = []  # names
        for text in sources:
            for line in text.splitlines():
                m = UNIFORM_LINE.match(line)
                if not m:
                    continue
                typ = m.group(4)
                for var in [v.strip() for v in m.group(5).split(',')]:
                    if typ in OPAQUE:
                        if var not in self.samplers:
                            self.samplers.append(var)
                    elif typ in TYPES:
                        if var not in [u[0] for u in self.uniforms]:
                            self.uniforms.append((var, typ))
                    else:
                        raise SystemExit(f'{name}: unsupported uniform type {typ} ({var})')
        self.layout = []  # (name, type, offset)
        offset = 0
        for var, typ in self.uniforms:
            size, align, _, _ = TYPES[typ]
            offset = (offset + align - 1) // align * align
            self.layout.append((var, typ, offset))
            offset += size
        self.size = (offset + 15) // 16 * 16 or 16

    def block(self):
        members = ''.join(f'    {typ} gsu_{var};\n' for var, typ, _ in self.layout) or '    vec4 gsu_unused;\n'
        return f'layout(std140, binding = 0) uniform GSU\n{{\n{members}}};\n'

    def rewrite(self, text):
        """the uam form of one stage's full source (already concatenated)"""
        lines = text.split('\n')
        out = []
        block_done = False
        for i, line in enumerate(lines):
            m = UNIFORM_LINE.match(line)
            if m and m.group(4) in OPAQUE:
                indent, layout, typ, rest = m.group(1), m.group(3), m.group(4), m.group(6)
                for var in [v.strip() for v in m.group(5).split(',')]:
                    binding = self.samplers.index(var)
                    out.append(f'{indent}layout(binding = {binding}) uniform {typ} {var};{rest if var else ""}')
                continue
            if m and m.group(4) in TYPES:
                for var in [v.strip() for v in m.group(5).split(',')]:
                    out.append(f'{m.group(1)}#define {var} (gsu_{var})')
                continue
            out.append(line)
            # the block right after #version and the extension lines (before any use)
            if not block_done and line.startswith('#version'):
                out.append(self.block())
                block_done = True
        return '\n'.join(out)


# The presentation scaler (gs_dk.cpp Present): the composed frame onto the 1280x720 swapchain image, a fullscreen
# triangle from gl_VertexID. 5-tap Catmull-Rom bicubic + contrast-adaptive sharpen (as the PC host scaler,
# gs_host_present.cpp kScalerFragment) when the frame is magnified, one bilinear tap otherwise. uRect maps the output
# pixel to the frame's texture coordinate (scale x, scale y, offset x, offset y: the Y flip is in it).
SCALE_VS = """#version 450
uniform vec4 uRect;
out vec2 vUV;
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vUV = p * uRect.xy + uRect.zw;
}
"""
SCALE_FS = """#version 450
in vec2 vUV;
uniform sampler2D uFrame;
uniform vec2 uSrcSize;
uniform float uSharpen;
uniform float uPlain;
out vec4 oColor;
vec4 tex(vec2 uv) { return texture(uFrame, uv); }
void main()
{
    if (uPlain > 0.5)
    {
        oColor = vec4(texture(uFrame, vUV).rgb, 1.0);
        return;
    }
    vec2 pos = vUV * uSrcSize;
    vec2 center = floor(pos - 0.5) + 0.5;
    vec2 f = pos - center;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    vec2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    vec2 w3 = 0.5 * f3 - 0.5 * f2;
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 tc0 = (center - 1.0) / uSrcSize;
    vec2 tc3 = (center + 2.0) / uSrcSize;
    vec2 tc12 = (center + offset12) / uSrcSize;
    vec4 c = tex(vec2(tc12.x, tc0.y)) * (w12.x * w0.y) + tex(vec2(tc0.x, tc12.y)) * (w0.x * w12.y)
           + tex(vec2(tc12.x, tc12.y)) * (w12.x * w12.y) + tex(vec2(tc3.x, tc12.y)) * (w3.x * w12.y)
           + tex(vec2(tc12.x, tc3.y)) * (w12.x * w3.y);
    float norm = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    c /= norm;
    vec2 px = 1.0 / uSrcSize;
    vec3 n = tex(vUV + vec2(0.0, -px.y)).rgb, s = tex(vUV + vec2(0.0, px.y)).rgb;
    vec3 e = tex(vUV + vec2(px.x, 0.0)).rgb, w = tex(vUV + vec2(-px.x, 0.0)).rgb;
    vec3 lo = min(min(n, s), min(e, w)), hi = max(max(n, s), max(e, w));
    vec3 amp = clamp(min(lo, 1.0 - hi) / max(hi, vec3(0.0001)), 0.0, 1.0);
    float peak = -1.0 / mix(8.0, 5.0, uSharpen);
    vec3 wgt = sqrt(amp) * peak;
    vec3 sharp = (c.rgb + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt);
    oColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
"""
SCALE_IDS = {'vert': 1, 'frag': 2}  # gs_dk_shaders.h kScaleVertexId / kScaleFragmentId


def draw_variant_head(fs, key):
    """gs_hw_backend.cpp drawVariantFor: the defines after the #version line"""
    field = lambda shift, mask: (key >> shift) & mask  # noqa: E731
    defines = '#define GS_SPECIALIZED 1\n'
    ztex = field(3, 3)
    for name, value in (('uTme', field(0, 1)), ('uReinterp', field(1, 1)), ('uIndexed', field(2, 1)),
                        ('uZTex', 0 if ztex == 0 else 16 if ztex == 1 else 32), ('uLinear', field(5, 1)),
                        ('uRtTex24', field(6, 3)), ('uTfx', field(8, 3)), ('uTcc', field(10, 1)),
                        ('uFst', field(11, 1)), ('uIip', field(12, 1)), ('uPass', field(13, 3)),
                        ('uSwBlend', field(15, 1)), ('uZAlias', field(16, 7)), ('uZAliasWrite', field(19, 1)),
                        ('uDate', field(20, 1))):
        defines += f'#define {name} {value}\n'
    if field(21, 1):
        defines += '#define GS_DUAL_SOURCE 1\n'
    cut = fs.index('\n') + 1
    return fs[:cut] + defines + fs[cut:]


def programs(ext, keys):
    """[(family name, [(stage, original source)])] of every program the backend builds"""
    k = raw_strings(os.path.join(ext, 'ps2xRuntime', 'src', 'lib', 'gs', 'gpu', 'gs_hw_backend.cpp'))
    draw_fs = [k['kFsSource'] + k['kReinterpretSource'] + k['kFsTail']]
    draw_fs += [draw_variant_head(k['kFsSource'], key) + k['kReinterpretSource'] + k['kFsTail'] for key in sorted(keys)]
    return [
        ('draw', [('vert', k['kVsSource'])] + [('frag', s) for s in draw_fs]),
        ('shuffle', [('vert', k['kShuffleVs']), ('frag', k['kShuffleFs'])]),
        ('present', [('vert', k['kPresentVs']), ('frag', k['kPresentFs'])]),
        ('clut', [('vert', k['kPresentVs']), ('frag', k['kClutFsHead'] + k['kReinterpretSource'] + k['kClutFsMain'])]),
        ('pack', [('comp', k['kPackCs'])]),
        ('scale', [('vert', SCALE_VS), ('frag', SCALE_FS)]),
    ] + ([
        # stencil shadow volumes (GOW_GS_HW_STENCILVOL): constant bit draws, depth copy into a stencil format
        ('svol', [('vert', k['kPresentVs']), ('frag', k['kStencilOneFs']), ('frag', k['kZCopyFs'])]),
    ] if 'kStencilOneFs' in k else [])


def wsl_path(path):
    path = os.path.abspath(path).replace('\\', '/')
    return '/mnt/' + path[0].lower() + path[2:]


def compile_all(items, work):
    """uam over every (key, stage, text) in one WSL call; {key: bytes or error text}"""
    os.makedirs(work, exist_ok=True)
    script = ['set +e']
    for key, stage, text in items:
        src = os.path.join(work, f'{key}.{stage}.glsl')
        open(src, 'w', newline='\n').write(text)
        dst = os.path.join(work, f'{key}.dksh')
        if os.path.exists(dst):
            os.remove(dst)
        script.append(f"{WSL_UAM} -s {stage} -o '{wsl_path(dst)}' '{wsl_path(src)}' > '{wsl_path(dst)}.log' 2>&1")
    sh = os.path.join(work, 'compile.sh')
    open(sh, 'w', newline='\n').write('\n'.join(script) + '\n')
    subprocess.run(['wsl.exe', '-d', 'Ubuntu', '-u', 'root', '--', 'bash', wsl_path(sh)],
                   env=dict(os.environ, MSYS_NO_PATHCONV='1'), check=False)
    out = {}
    for key, stage, _ in items:
        dst = os.path.join(work, f'{key}.dksh')
        if os.path.exists(dst) and os.path.getsize(dst) > 0:
            out[key] = open(dst, 'rb').read()
        else:
            out[key] = open(dst + '.log', errors='replace').read() if os.path.exists(dst + '.log') else 'no output'
    return out


def read_keys(path):
    if path == '-':
        return set()
    text = open(path, errors='replace').read()
    return {int(k, 16) for k in re.findall(r'key=([0-9a-fA-F]{6,8})', text)} | \
        {int(k, 16) for k in re.findall(r'^\s*([0-9a-fA-F]{6,8})\s*$', text, re.M)}


def main(argv):
    if len(argv) < 4:
        sys.exit(__doc__)
    ext, keys_file, out_cpp = argv[1], argv[2], argv[3]
    work = argv[argv.index('--work') + 1] if '--work' in argv else os.path.join(os.path.dirname(out_cpp), 'dk_work')
    keys = read_keys(keys_file)
    families, items, entries = [], [], []
    for name, stages in programs(ext, keys):
        fam = Family(name, [text for _, text in stages])
        families.append(fam)
        for stage, text in stages:
            h = SCALE_IDS[stage] if name == 'scale' else fnv64(text)
            key = f'{h:016x}'
            items.append((key, stage, fam.rewrite(text)))
            entries.append((key, h, stage, len(families) - 1))
    # one entry per distinct source (the draw VS appears once, shared sources compile once)
    seen, unique = set(), []
    for item, entry in zip(items, entries):
        if item[0] not in seen:
            seen.add(item[0])
            unique.append((item, entry))
    results = compile_all([u[0] for u in unique], work)
    failed = [(e[0], results[e[0]]) for _, e in unique if isinstance(results[e[0]], str)]
    for key, log in failed:
        print(f'uam failed {key}: {log.strip()[:400]}')
    stage_id = {'vert': 0, 'frag': 1, 'comp': 2}
    type_id = {t: i for i, t in enumerate(TYPES)}
    lines = ['// Generated by tools/dk_shaders.py: deko3d (uam) builds of the GS renderer programs. Do not edit.',
             '#include "gs_dk_shaders.h"', '', 'namespace GSDK', '{']
    for i, fam in enumerate(families):
        lines.append(f'static const UniformInfo kUniforms{i}[] = {{')
        for var, typ, off in fam.layout:
            lines.append(f'    {{"{var}", {off}u, {type_id[typ]}u}},')
        lines.append('    {nullptr, 0u, 0u}};')
        lines.append(f'static const char *const kSamplers{i}[] = {{' + ''.join(f'"{s}", ' for s in fam.samplers) + 'nullptr};')
    for (item, entry) in unique:
        key = entry[0]
        if isinstance(results[key], str):
            continue
        data = results[key]
        body = ','.join(str(b) for b in data)
        lines.append(f'alignas(256) static const unsigned char kCode_{key}[] = {{{body}}};')
    lines.append('const Family kFamilies[] = {')
    for i, fam in enumerate(families):
        lines.append(f'    {{"{fam.name}", kUniforms{i}, kSamplers{i}, {fam.size}u}},')
    lines.append('};')
    lines.append('const ShaderBinary kShaders[] = {')
    for (item, entry) in unique:
        key, h, stage, fam = entry
        if isinstance(results[key], str):
            continue
        lines.append(f'    {{0x{h:016x}ull, {stage_id[stage]}u, {fam}u, kCode_{key}, sizeof(kCode_{key})}},')
    lines.append('};')
    lines.append(f'const unsigned kShaderCount = {len(unique) - len(failed)}u;')
    lines.append('const char *const kTypeNames[] = {' + ''.join(f'"{t}", ' for t in TYPES) + '};')
    lines.append('} // namespace GSDK')
    open(out_cpp, 'w', newline='\n').write('\n'.join(lines) + '\n')
    total = sum(len(results[e[0]]) for _, e in unique if not isinstance(results[e[0]], str))
    print(f'{out_cpp}: {len(unique) - len(failed)} shaders ({total >> 10} KB), {len(failed)} failed, '
          f'{len(keys)} draw variants; families: ' + ', '.join(f'{f.name} {f.size} B/{len(f.samplers)} samplers' for f in families))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
