// GOW-Port: MSL translation of the GLSL shaders of gs_hw_backend.cpp (kVsSource, kFsSource + kReinterpretSource + kFsTail,
// kShuffleVs/Fs, kPresentVs/Fs, kClutFs). Comments on the GS semantics live with the GLSL originals.
// Metal differences: NDC y points up while texture row 0 is the top, so the vertex shaders flip y and texture row = PS2 row
// as in the GL renderer; Metal's flat attributes use the first vertex, GL's the last, so the flat colour is read from the
// triangle's last vertex; destination reads use framebuffer fetch ([[color(0)]] input) instead of a texture + barrier.
R"MSL(
#include <metal_stdlib>
using namespace metal;

struct HwVertex { float x, y, z, r, g, b, a, s, t, q, u, v, cu0, cv0, cu1, cv1; };
struct VsUniforms { float invW, invH; };

// mirrors FsUniforms in gs_metal_backend.mm (scalars only: identical layout on both sides)
struct FsUniforms
{
    int tme, fst, tfx, tcc;
    int iip, indexed, linear, reinterp;
    int wrapX, wrapY, rtTex24, aem;
    int ate, atst, aref, pass;
    int date, datm, swBlend, colClamp;
    int abcd[4];
    int zAlias, zAliasWrite, zTex, failTint;
    int hwBlend, hwSf, hwDf, hwEq; // fixed-function blend done in the shader (framebuffer-fetch variant)
    float fix, zAliasMax, zMax, blendScale;
    float ta0, ta1, texDimX, texDimY;
    float texSizeX, texSizeY, rtPxX, rtPxY;
    float blendColor[4];
    int texFmt[4], texFmt2[4], rtFmt[4], rtFmt2[4], tex0[4];
};

struct VsOut
{
    float4 pos [[position]];
    float4 col [[center_no_perspective]];
    float4 colFlat [[flat]];
    float3 stq [[center_no_perspective]];
    float2 uv [[center_no_perspective]];
    float4 uvClamp [[flat]];
};

vertex VsOut vs_main(uint vid [[vertex_id]], const device HwVertex *verts [[buffer(0)]], constant VsUniforms &vu [[buffer(1)]])
{
    const HwVertex v = verts[vid];
    const HwVertex last = verts[vid - vid % 3u + 2u];
    VsOut o;
    const float2 p = float2(v.x * vu.invW, v.y * vu.invH) * 2.0 - 1.0;
    o.pos = float4(p.x, -p.y, v.z, 1.0);
    o.col = float4(v.r, v.g, v.b, v.a);
    o.colFlat = float4(last.r, last.g, last.b, last.a);
    o.stq = float3(v.s, v.t, v.q);
    o.uv = float2(v.u, v.v);
    o.uvClamp = float4(v.cu0, v.cv0, v.cu1, v.cv1);
    return o;
}

constexpr sampler kNearest(coord::normalized, filter::nearest, address::clamp_to_edge);

static uint tab(const device ushort *tables, uint i) { return uint(tables[i]); }

static float4 reinterpretTexel(int2 p, constant FsUniforms &u, const device ushort *tables, texture2d<float> rtColor, constant uint *rpal)
{
    uint x = uint(p.x), y = uint(p.y);
    uint psx = uint(u.texFmt[0]), psy = uint(u.texFmt[1]);
    uint pagesPerRow = (uint(u.tex0[1]) * 64u) >> psx;
    uint page = (uint(u.tex0[0]) >> 5) + (y >> psy) * pagesPerRow + (x >> psx);
    uint entry = ((uint(u.tex0[0]) & 31u) << (psx + psy)) + ((y & ((1u << psy) - 1u)) << psx) + (x & ((1u << psx) - 1u));
    uint pixelIndex = (page << uint(u.texFmt[2])) + tab(tables, uint(u.texFmt2[2]) + entry);
    uint bitpos = (pixelIndex << uint(u.texFmt[3])) + uint(u.texFmt2[0]);
    uint B = uint(u.rtFmt[3]);
    uint rtIdx = bitpos / B;
    uint within = bitpos % B;
    uint rtBase = uint(u.rtFmt2[1]) << uint(u.rtFmt[2]);
    if (rtIdx < rtBase) return float4(1.0, 0.0, 1.0, 1.0);
    uint rel = rtIdx - rtBase;
    uint rpage = rel >> uint(u.rtFmt[2]);
    uint inpage = rel & ((1u << uint(u.rtFmt[2])) - 1u);
    uint e = tab(tables, uint(u.rtFmt2[0]) + inpage);
    uint rpsx = uint(u.rtFmt[0]), rpsy = uint(u.rtFmt[1]);
    uint fbw = uint(u.rtFmt2[2]);
    uint px = (rpage % fbw) * (1u << rpsx) + (e & ((1u << rpsx) - 1u));
    uint py = (rpage / fbw) * (1u << rpsy) + (e >> rpsx);
    float4 c = rtColor.sample(kNearest, (float2(float(px), float(py)) + 0.5) / float2(u.rtPxX, u.rtPxY)) * 255.0;
    c = floor(c + 0.5);
    uint word;
    if (B == 32u)
        word = uint(c.r) | (uint(c.g) << 8) | (uint(c.b) << 16) | (uint(c.a) << 24);
    else
        word = (c.a >= 128.0 ? 0x8000u : 0u) | ((uint(c.b) >> 3) << 10) | ((uint(c.g) >> 3) << 5) | (uint(c.r) >> 3);
    uint bits = uint(u.texFmt2[1]);
    uint value = (bits >= 32u) ? (word >> within) : ((word >> within) & ((1u << bits) - 1u));
    int kind = u.texFmt2[3];
    if (kind == 0) return float4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), float((value >> 24) & 255u)) / 255.0;
    if (kind == 1) return float4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), 128.0) / 255.0;
    if (kind == 2)
    {
        float3 rgb = float3(float(value & 31u), float((value >> 5) & 31u), float((value >> 10) & 31u)) * 8.0;
        return float4(rgb, ((value & 0x8000u) != 0u) ? 128.0 : 0.0) / 255.0;
    }
    return unpack_unorm4x8_to_float(rpal[value & 255u]);
}

static float4 indexedTexel(int2 p, constant FsUniforms &u, texture2d<float> tex, const device uint *ipal)
{
    // texture sizes are powers of two (1 << TW / TH), so REPEAT is a mask (also for negative texels); an integer % is
    // a long instruction sequence on the GPU and this runs four times per bilinear fragment
    int2 size = max(int2(float2(u.texDimX, u.texDimY)), int2(1));
    p.x = (u.wrapX == 0) ? (p.x & (size.x - 1)) : clamp(p.x, 0, size.x - 1);
    p.y = (u.wrapY == 0) ? (p.y & (size.y - 1)) : clamp(p.y, 0, size.y - 1);
    int idx = int(tex.read(uint2(p)).r * 255.0 + 0.5);
    return unpack_unorm4x8_to_float(ipal[idx & 255]);
}

static float clamp255(float v) { return clamp(floor(v), 0.0, 255.0); }

static bool alphaPasses(float a, int atst, int aref)
{
    int ai = int(a);
    switch (atst)
    {
    case 0: return false;
    case 1: return true;
    case 2: return ai < aref;
    case 3: return ai <= aref;
    case 4: return ai == aref;
    case 5: return ai >= aref;
    case 6: return ai > aref;
    default: return ai != aref;
    }
}

static float3 blendSel(int sel, float3 cs, float3 cd) { return sel == 0 ? cs : sel == 1 ? cd : float3(0.0); }

struct Shaded { float4 color; float4 blend; };

// Specialisation of fs_main / fs_fetch (pipelineFor): the texture path and the per-draw switches as function constants, so
// a draw's pipeline holds only its own code (the uber-shader's register footprint is that of its largest path). -1: generic,
// everything read from FsUniforms (used until the specialised pipeline has compiled in the background).
// kTexPath: 0 untextured, 1 direct / render target, 2 paletted, 3 reinterpreted target, 4 depth target.
// kFlags: 1 linear, 2 swBlend, 4 zAlias, 8 date, 16 alpha-test pass.
constant int kTexPath [[function_constant(0)]];
constant int kFlags [[function_constant(1)]];

static Shaded shade(VsOut in, float4 dest, constant FsUniforms &u, texture2d<float> tex, depth2d<float> ztex,
                    texture2d<float> rtColor, sampler smp, const device ushort *tables, constant uint *rpal, const device uint *ipal)
{
    const bool spec = kTexPath >= 0;
    const int texPath = spec ? kTexPath : (u.tme == 0 ? 0 : u.reinterp != 0 ? 3 : u.indexed != 0 ? 2 : u.zTex != 0 ? 4 : 1);
    const bool linear = spec ? (kFlags & 1) != 0 : u.linear != 0;
    const bool swBlend = spec ? (kFlags & 2) != 0 : u.swBlend != 0;
    const bool zAlias = spec ? (kFlags & 4) != 0 : u.zAlias != 0;
    const bool date = spec ? (kFlags & 8) != 0 : u.date != 0;
    const bool testPass = spec ? (kFlags & 16) != 0 : u.pass != 0;
    uint zFrag = uint(clamp(in.pos.z, 0.0, 1.0) * u.zAliasMax + 0.5);
    if (zAlias)
    {
        float4 d = floor(dest * 255.0 + 0.5);
        uint zd = uint(d.r) | (uint(d.g) << 8) | (uint(d.b) << 16);
        int ztst = u.zAlias - 1;
        bool pass = ztst == 1 || (ztst == 2 && zFrag >= zd) || (ztst == 3 && zFrag > zd);
        if (!pass) discard_fragment();
    }
    if (date)
    {
        bool bit = dest.a >= 0.5;
        if (bit != (u.datm != 0)) discard_fragment();
    }
    float4 v = (u.iip != 0) ? in.col : in.colFlat;
    float3 rgb = v.rgb;
    float a = v.a;
    if (texPath != 0)
    {
        float q = (abs(in.stq.z) < 1e-30) ? 1e-30 : in.stq.z;
        float2 texDim = float2(u.texDimX, u.texDimY);
        float2 texSize = float2(u.texSizeX, u.texSizeY);
        float2 uv = (u.fst != 0) ? in.uv : (in.stq.xy / q) * texDim;
        uv = clamp(uv, in.uvClamp.xy, in.uvClamp.zw);
        if (!linear)
            uv = floor(uv * 16.0 + 0.015625) / 16.0;
        float4 t;
        if (texPath == 3)
        {
            int2 ip = int2(floor(uv));
            ip = int2(clamp(ip.x, 0, u.tex0[2] - 1), clamp(ip.y, 0, u.tex0[3] - 1));
            t = reinterpretTexel(ip, u, tables, rtColor, rpal) * 255.0;
        }
        else if (texPath == 2)
        {
            if (linear)
            {
                float2 f = uv - 0.5;
                int2 i0 = int2(floor(f));
                float2 w = f - floor(f);
                t = mix(mix(indexedTexel(i0, u, tex, ipal), indexedTexel(i0 + int2(1, 0), u, tex, ipal), w.x),
                        mix(indexedTexel(i0 + int2(0, 1), u, tex, ipal), indexedTexel(i0 + int2(1, 1), u, tex, ipal), w.x), w.y) * 255.0;
            }
            else
                t = indexedTexel(int2(floor(uv)), u, tex, ipal) * 255.0;
        }
        else if (texPath == 4)
        {
            float2 zhost = float2(float(ztex.get_width()), float(ztex.get_height()));
            float zf = ztex.sample(kNearest, (floor(uv * zhost / texSize + 0.001) + 0.5) / zhost);
            uint zi = uint(clamp(zf, 0.0, 1.0) * u.zMax + 0.5);
            if (u.zTex == 16)
            {
                float3 zrgb = float3(float(zi & 31u), float((zi >> 5) & 31u), float((zi >> 10) & 31u)) * 8.0;
                t = float4(zrgb, ((zi & 0x8000u) != 0u) ? 128.0 : 0.0);
            }
            else
                t = float4(float(zi & 255u), float((zi >> 8) & 255u), float((zi >> 16) & 255u), 128.0);
        }
        else if (!linear)
        {
            float2 host = float2(float(tex.get_width()), float(tex.get_height()));
            t = tex.sample(smp, (floor(uv * host / texSize + 0.001) + 0.5) / host) * 255.0;
        }
        else
            t = tex.sample(smp, uv / texSize) * 255.0;
        t = floor(t + 0.5);
        if (u.rtTex24 == 1)
            t.a = (u.aem != 0 && t.r + t.g + t.b == 0.0) ? 0.0 : u.ta0;
        else if (u.rtTex24 == 2)
            t.a = (u.aem != 0 && t.r + t.g + t.b == 0.0) ? 0.0 : ((t.a >= 128.0) ? u.ta1 : u.ta0);
        float ta = (u.tcc != 0) ? t.a : v.a;
        if (u.tfx == 0)
        {
            rgb = float3(clamp255(t.r * v.r / 128.0), clamp255(t.g * v.g / 128.0), clamp255(t.b * v.b / 128.0));
            a = (u.tcc != 0) ? clamp255(t.a * v.a / 128.0) : v.a;
        }
        else if (u.tfx == 1)
        {
            rgb = t.rgb;
            a = ta;
        }
        else if (u.tfx == 2)
        {
            rgb = float3(clamp255(t.r * v.r / 128.0 + v.a), clamp255(t.g * v.g / 128.0 + v.a), clamp255(t.b * v.b / 128.0 + v.a));
            a = (u.tcc != 0) ? clamp255(t.a + v.a) : v.a;
        }
        else
        {
            rgb = float3(clamp255(t.r * v.r / 128.0 + v.a), clamp255(t.g * v.g / 128.0 + v.a), clamp255(t.b * v.b / 128.0 + v.a));
            a = ta;
        }
    }
    if (testPass)
    {
        bool pass = (u.ate == 0) || alphaPasses(a, u.atst, u.aref);
        if (u.pass == 1 && !pass) discard_fragment();
        if (u.pass == 2 && pass) discard_fragment();
        if (u.pass == 2 && u.failTint != 0) { rgb = float3(255.0, 0.0, 255.0); a = 128.0; }
    }
    if (swBlend)
    {
        float4 d = floor(dest * 255.0 + 0.5);
        float3 cs = floor(rgb);
        float c = u.abcd[2] == 0 ? a : u.abcd[2] == 1 ? d.a : u.fix;
        float3 r = floor((blendSel(u.abcd[0], cs, d.rgb) - blendSel(u.abcd[1], cs, d.rgb)) * c / 128.0) + blendSel(u.abcd[3], cs, d.rgb);
        rgb = (u.colClamp != 0) ? clamp(r, 0.0, 255.0) : r - 256.0 * floor(r / 256.0);
    }
    if (zAlias && u.zAliasWrite != 0)
        rgb = float3(float(zFrag & 255u), float((zFrag >> 8) & 255u), float((zFrag >> 16) & 255u));
    Shaded s;
    s.color = float4(rgb / 255.0, a / 255.0);
    s.blend = float4(min(a * u.blendScale, 1.0));
    return s;
}

struct FsOutDual
{
    float4 color [[color(0), index(0)]];
    float4 blend [[color(0), index(1)]];
};

fragment FsOutDual fs_main(VsOut in [[stage_in]], constant FsUniforms &u [[buffer(0)]], const device ushort *tables [[buffer(1)]],
                           constant uint *rpal [[buffer(2)]], const device uint *ipal [[buffer(3)]],
                           texture2d<float> tex [[texture(0)]], depth2d<float> ztex [[texture(1)]], texture2d<float> rtColor [[texture(2)]],
                           sampler smp [[sampler(0)]])
{
    Shaded s = shade(in, float4(0.0), u, tex, ztex, rtColor, smp, tables, rpal, ipal);
    FsOutDual o;
    o.color = s.color;
    o.blend = s.blend;
    return o;
}

// 0 zero, 1 one, 2 src1 alpha, 3 1 - src1 alpha, 4 constant, 5 1 - constant
static float3 blendFactor(int f, float a, float3 k)
{
    switch (f)
    {
    case 0: return float3(0.0);
    case 1: return float3(1.0);
    case 2: return float3(a);
    case 3: return float3(1.0 - a);
    case 4: return k;
    default: return float3(1.0) - k;
    }
}

// reads the destination pixel (DATE, shader blend, Z in the colour memory); fixed-function blending, when the draw also
// needs it, is done here on the fetched colour
fragment float4 fs_fetch(VsOut in [[stage_in]], float4 dest [[color(0), raster_order_group(0)]], constant FsUniforms &u [[buffer(0)]],
                         const device ushort *tables [[buffer(1)]], constant uint *rpal [[buffer(2)]], const device uint *ipal [[buffer(3)]],
                         texture2d<float> tex [[texture(0)]], depth2d<float> ztex [[texture(1)]], texture2d<float> rtColor [[texture(2)]],
                         sampler smp [[sampler(0)]])
{
    Shaded s = shade(in, dest, u, tex, ztex, rtColor, smp, tables, rpal, ipal);
    if (u.hwBlend == 0)
        return s.color;
    float4 src = clamp(s.color, 0.0, 1.0);
    float3 k = float3(u.blendColor[0], u.blendColor[1], u.blendColor[2]);
    float3 sf = blendFactor(u.hwSf, s.blend.a, k), df = blendFactor(u.hwDf, s.blend.a, k);
    float3 r = (u.hwEq == 0) ? src.rgb * sf + dest.rgb * df : dest.rgb * df - src.rgb * sf;
    return float4(clamp(r, 0.0, 1.0), src.a);
}

// ---- channel shuffles
struct ShVertex { float x, y, ox, oy; };
struct ShVsOut { float4 pos [[position]]; float2 off [[flat]]; };
struct ShUniforms { int srcZ, hs, hd, pad0; float zMax, scaleX, scaleY, pad1; int texa[4]; };

vertex ShVsOut vs_shuffle(uint vid [[vertex_id]], const device ShVertex *verts [[buffer(0)]], constant VsUniforms &vu [[buffer(1)]])
{
    const ShVertex v = verts[vid];
    ShVsOut o;
    const float2 p = float2(v.x * vu.invW, v.y * vu.invH) * 2.0 - 1.0;
    o.pos = float4(p.x, -p.y, 0.0, 1.0);
    o.off = float2(v.ox, v.oy);
    return o;
}

fragment float4 fs_shuffle(ShVsOut in [[stage_in]], constant ShUniforms &u [[buffer(0)]], texture2d<float> src [[texture(0)]],
                           depth2d<float> srcDepth [[texture(1)]])
{
    int2 p = int2(in.pos.xy) + int2(round(in.off * float2(u.scaleX, u.scaleY)));
    float4 c;
    if (u.srcZ != 0)
    {
        uint z = uint(clamp(srcDepth.read(uint2(p)), 0.0, 1.0) * u.zMax + 0.5);
        c = float4(float(z & 255u), float((z >> 8) & 255u), float((z >> 16) & 255u), float(z >> 24)) / 255.0;
    }
    else
        c = src.read(uint2(p));
    float2 h = (u.hs == 0) ? c.rg : c.ba;
    uint v = uint(h.x * 255.0 + 0.5) | (uint(h.y * 255.0 + 0.5) << 8);
    uint rgb = v & 0x7FFFu;
    uint a = (u.texa[2] != 0 && rgb == 0u) ? 0u : uint((v & 0x8000u) != 0u ? u.texa[1] : u.texa[0]);
    v = rgb | (a << 15);
    h = float2(float(v & 255u), float(v >> 8)) / 255.0;
    return (u.hd == 0) ? float4(h, 0.0, 0.0) : float4(0.0, 0.0, h);
}

// ---- full-screen passes: display output, solid colour, CLUT fetch
struct FullOut { float4 pos [[position]]; };

vertex FullOut vs_full(uint vid [[vertex_id]])
{
    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
    FullOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

struct PresentUniforms { int offX, offY, ct24, alphaX2; int edgeX, edgeY, pad0, pad1; };

fragment float4 fs_present(FullOut in [[stage_in]], constant PresentUniforms &u [[buffer(0)]], texture2d<float> src [[texture(0)]])
{
    int2 p = max(int2(in.pos.xy) + int2(u.offX, u.offY), int2(u.edgeX, u.edgeY));
    int2 size = int2(int(src.get_width()), int(src.get_height()));
    float4 c = (p.x < size.x && p.y < size.y) ? src.read(uint2(p)) : float4(0.0);
    float a = (u.ct24 != 0) ? 128.0 / 255.0 : c.a;
    return float4(c.rgb, (u.alphaX2 != 0) ? min(1.0, a * 2.0) : a);
}

// window output (CAMetalLayer): the composed frame scaled into its 16:9 rectangle (the viewport) with the host's
// scaler (gs_host_present.cpp kScalerFragment): 5-tap Catmull-Rom bicubic + contrast-adaptive sharpen when magnified
// (sharpen < 0: bicubic only), one bilinear tap otherwise. The output chain is in gs_metal_post.mm.
struct ScaleUniforms { float2 origin, size, srcSize; float sharpen, plain; };
constexpr sampler kLinear(coord::normalized, filter::linear, address::clamp_to_edge);

fragment float4 fs_scale(FullOut in [[stage_in]], constant ScaleUniforms &u [[buffer(0)]], texture2d<float> src [[texture(0)]])
{
    float2 uvIn = (in.pos.xy - u.origin) / u.size;
    if (u.plain > 0.5)
        return float4(src.sample(kLinear, uvIn).rgb, 1.0);
    float2 pos = uvIn * u.srcSize;
    float2 center = floor(pos - 0.5) + 0.5;
    float2 f = pos - center;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    float2 w3 = 0.5 * f3 - 0.5 * f2;
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    float2 tc0 = (center - 1.0) / u.srcSize;
    float2 tc3 = (center + 2.0) / u.srcSize;
    float2 tc12 = (center + offset12) / u.srcSize;
    float4 c = src.sample(kLinear, float2(tc12.x, tc0.y)) * (w12.x * w0.y)
             + src.sample(kLinear, float2(tc0.x, tc12.y)) * (w0.x * w12.y)
             + src.sample(kLinear, float2(tc12.x, tc12.y)) * (w12.x * w12.y)
             + src.sample(kLinear, float2(tc3.x, tc12.y)) * (w3.x * w12.y)
             + src.sample(kLinear, float2(tc12.x, tc3.y)) * (w12.x * w3.y);
    float norm = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    c /= norm;
    if (u.sharpen < 0.0)
        return float4(clamp(c.rgb, 0.0, 1.0), 1.0); // the sharpen runs later in the chain (fs_cas) or is off
    float2 px = 1.0 / u.srcSize;
    float3 n = src.sample(kLinear, uvIn + float2(0.0, -px.y)).rgb, s = src.sample(kLinear, uvIn + float2(0.0, px.y)).rgb;
    float3 e = src.sample(kLinear, uvIn + float2(px.x, 0.0)).rgb, w = src.sample(kLinear, uvIn + float2(-px.x, 0.0)).rgb;
    float3 lo = min(min(n, s), min(e, w)), hi = max(max(n, s), max(e, w));
    float3 amp = clamp(min(lo, 1.0 - hi) / max(hi, float3(0.0001)), 0.0, 1.0);
    float peak = -1.0 / mix(8.0, 5.0, u.sharpen);
    float3 wgt = sqrt(amp) * peak;
    float3 sharp = (c.rgb + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt);
    return float4(clamp(sharp, 0.0, 1.0), 1.0);
}

static float fxLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// GOW_GS_MTL_AA=fxaa: FXAA 3.11 (Lottes; the quality-preset walk as in Simon Rodriguez's write-up) on the composed frame at
// render resolution, before scaling; subpixel blend 0.5 (HUD text stays readable)
fragment float4 fs_fxaa(FullOut in [[stage_in]], constant ScaleUniforms &u [[buffer(0)]], texture2d<float> src [[texture(0)]])
{
    const float kQuality[12] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    float2 px = 1.0 / u.srcSize;
    float2 uv = (in.pos.xy - u.origin) / u.size;
    float3 rgbM = src.sample(kLinear, uv).rgb;
    float lM = fxLuma(rgbM);
    float lN = fxLuma(src.sample(kLinear, uv + float2(0.0, -px.y)).rgb), lS = fxLuma(src.sample(kLinear, uv + float2(0.0, px.y)).rgb);
    float lW = fxLuma(src.sample(kLinear, uv + float2(-px.x, 0.0)).rgb), lE = fxLuma(src.sample(kLinear, uv + float2(px.x, 0.0)).rgb);
    float lMin = min(lM, min(min(lN, lS), min(lW, lE))), lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125))
        return float4(rgbM, 1.0);
    float lNW = fxLuma(src.sample(kLinear, uv + float2(-px.x, -px.y)).rgb), lNE = fxLuma(src.sample(kLinear, uv + float2(px.x, -px.y)).rgb);
    float lSW = fxLuma(src.sample(kLinear, uv + float2(-px.x, px.y)).rgb), lSE = fxLuma(src.sample(kLinear, uv + float2(px.x, px.y)).rgb);
    float lNS = lN + lS, lWE = lW + lE;
    float lWC = lNW + lSW, lEC = lNE + lSE, lNC = lNW + lNE, lSC = lSW + lSE;
    float edgeH = abs(-2.0 * lW + lWC) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * lE + lEC);
    float edgeV = abs(-2.0 * lN + lNC) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * lS + lSC);
    bool horz = edgeH >= edgeV;
    float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool steep1 = abs(g1) >= abs(g2);
    float gScaled = 0.25 * max(abs(g1), abs(g2));
    float step = horz ? px.y : px.x;
    float lLocal;
    if (steep1)
    {
        step = -step;
        lLocal = 0.5 * (l1 + lM);
    }
    else
        lLocal = 0.5 * (l2 + lM);
    float2 cur = uv;
    if (horz)
        cur.y += step * 0.5;
    else
        cur.x += step * 0.5;
    float2 off = horz ? float2(px.x, 0.0) : float2(0.0, px.y);
    float2 uv1 = cur - off, uv2 = cur + off;
    float e1 = fxLuma(src.sample(kLinear, uv1).rgb) - lLocal, e2 = fxLuma(src.sample(kLinear, uv2).rgb) - lLocal;
    bool r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
    if (!r1) uv1 -= off;
    if (!r2) uv2 += off;
    for (int i = 2; i < 12 && !(r1 && r2); ++i)
    {
        if (!r1) e1 = fxLuma(src.sample(kLinear, uv1).rgb) - lLocal;
        if (!r2) e2 = fxLuma(src.sample(kLinear, uv2).rgb) - lLocal;
        r1 = abs(e1) >= gScaled;
        r2 = abs(e2) >= gScaled;
        if (!r1) uv1 -= off * kQuality[i];
        if (!r2) uv2 += off * kQuality[i];
    }
    float d1 = horz ? (uv.x - uv1.x) : (uv.y - uv1.y), d2 = horz ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool dir1 = d1 < d2;
    float pixelOffset = -min(d1, d2) / (d1 + d2) + 0.5;
    bool centreSmaller = lM < lLocal;
    float finalOffset = (((dir1 ? e1 : e2) < 0.0) != centreSmaller) ? pixelOffset : 0.0;
    float lAvg = (1.0 / 12.0) * (2.0 * (lNS + lWE) + lWC + lEC);
    float sub1 = clamp(abs(lAvg - lM) / range, 0.0, 1.0);
    float sub2 = (-2.0 * sub1 + 3.0) * sub1 * sub1;
    finalOffset = max(finalOffset, sub2 * sub2 * 0.5);
    float2 fuv = uv;
    if (horz)
        fuv.y += finalOffset * step;
    else
        fuv.x += finalOffset * step;
    return float4(src.sample(kLinear, fuv).rgb, 1.0);
}

// GOW_SHARPEN (when not folded into fs_scale): AMD FidelityFX CAS, no-scaling path, on a frame at output size; u.sharpen is
// CAS's sharpness 0..1
fragment float4 fs_cas(FullOut in [[stage_in]], constant ScaleUniforms &u [[buffer(0)]], texture2d<float> src [[texture(0)]])
{
    float2 px = 1.0 / u.srcSize;
    float2 uv = (in.pos.xy - u.origin) / u.size;
    float3 a = src.sample(kLinear, uv + float2(-px.x, -px.y)).rgb, b = src.sample(kLinear, uv + float2(0.0, -px.y)).rgb;
    float3 c = src.sample(kLinear, uv + float2(px.x, -px.y)).rgb, d = src.sample(kLinear, uv + float2(-px.x, 0.0)).rgb;
    float3 e = src.sample(kLinear, uv).rgb, f = src.sample(kLinear, uv + float2(px.x, 0.0)).rgb;
    float3 g = src.sample(kLinear, uv + float2(-px.x, px.y)).rgb, h = src.sample(kLinear, uv + float2(0.0, px.y)).rgb;
    float3 i = src.sample(kLinear, uv + float2(px.x, px.y)).rgb;
    float3 mn = min(min(min(d, e), min(f, b)), h), mx = max(max(max(d, e), max(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, i)));
    mx += max(mx, max(max(a, c), max(g, i)));
    float3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, float3(0.0001)), 0.0, 1.0));
    float3 w = amp * (-1.0 / mix(8.0, 5.0, u.sharpen));
    return float4(clamp(((b + d + f + h) * w + e) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);
}

fragment float4 fs_color(FullOut in [[stage_in]], constant float4 &color [[buffer(0)]])
{
    return color;
}

fragment float4 fs_clut(FullOut in [[stage_in]], constant FsUniforms &u [[buffer(0)]], const device ushort *tables [[buffer(1)]],
                        constant uint *rpal [[buffer(2)]], constant ushort2 *coords [[buffer(4)]], texture2d<float> rtColor [[texture(2)]])
{
    uint2 c = uint2(coords[int(in.pos.x)]);
    return reinterpretTexel(int2(c), u, tables, rtColor, rpal);
}
)MSL"
