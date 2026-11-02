// sw-dips12: draw()'s extent loop, one chain (old) vs four chains (new): same maxX / maxY bits on random vertex sets
// (random bits incl. NaN / Inf / -0 / denormals, and ordinary coordinates), every size 0..67 and larger ones.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
struct V { float x, y, z; uint8_t r, g, b, a; float s, t, q, u, v, cu0, cv0, cu1, cv1; };
static void oldMax(const std::vector<V> &verts, float &ox, float &oy)
{
    float maxX = 0.0f, maxY = 0.0f;
    for (const V &v : verts) { maxX = std::max(maxX, v.x); maxY = std::max(maxY, v.y); }
    ox = maxX, oy = maxY;
}
static void newMax(const std::vector<V> &verts, float &ox, float &oy)
{
    float mx[4] = {0.0f, 0.0f, 0.0f, 0.0f}, my[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const size_t nv = verts.size(), n4 = nv & ~size_t(3);
    const V *pv = verts.data();
    for (size_t i = 0; i < n4; i += 4)
        for (int k = 0; k < 4; ++k) { mx[k] = std::max(mx[k], pv[i + k].x); my[k] = std::max(my[k], pv[i + k].y); }
    for (size_t i = n4; i < nv; ++i) { mx[0] = std::max(mx[0], pv[i].x); my[0] = std::max(my[0], pv[i].y); }
    ox = std::max(std::max(mx[0], mx[1]), std::max(mx[2], mx[3]));
    oy = std::max(std::max(my[0], my[1]), std::max(my[2], my[3]));
}
int main()
{
    std::mt19937_64 rng(12345);
    const float special[] = {0.0f, -0.0f, 1e-45f, -1e-45f, 3.4e38f, -3.4e38f, 1.0f / 0.0f, -1.0f / 0.0f, 0.0f / 0.0f, -(0.0f / 0.0f), 640.0f, 640.0f};
    uint64_t bad = 0, cases = 0;
    for (int it = 0; it < 2000000; ++it)
    {
        size_t n = it < 680000 ? static_cast<size_t>(it % 68) : static_cast<size_t>(rng() % 5000);
        std::vector<V> vs(n);
        const int mode = it % 3;
        for (V &v : vs)
            for (float *f : {&v.x, &v.y})
            {
                const uint64_t r = rng();
                if (mode == 0) { uint32_t b = static_cast<uint32_t>(r); std::memcpy(f, &b, 4); }
                else if (mode == 1) *f = static_cast<float>(static_cast<int64_t>(r % 20000) - 10000) / 7.0f;
                else *f = (r & 7) == 0 ? special[(r >> 3) % 12] : static_cast<float>(r % 4096) * 0.5f;
            }
        float ax, ay, bx, by;
        oldMax(vs, ax, ay);
        newMax(vs, bx, by);
        ++cases;
        if (std::memcmp(&ax, &bx, 4) || std::memcmp(&ay, &by, 4)) ++bad;
        if (it == 680000 - 1) n = 0;
    }
    std::printf("cases %llu differing %llu\n", (unsigned long long)cases, (unsigned long long)bad);
    return bad != 0;
}
