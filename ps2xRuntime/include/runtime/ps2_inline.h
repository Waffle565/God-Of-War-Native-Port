#ifndef PS2_INLINE_H
#define PS2_INLINE_H

// GOW-Port: the small helpers the generated code calls on every guest instruction (memory access, lane extracts,
// GPR writes, FPU/VU float rules) must be inlined; MSVC stops inlining inside the large generated functions and
// left them as calls (several % of the EE thread). Their rare slow paths are PS2_NOINLINE so the inlined part stays small.
#if defined(_MSC_VER)
#define PS2_FORCEINLINE __forceinline
#define PS2_NOINLINE __declspec(noinline)
#define PS2_LIKELY(x) (x)
#else
#define PS2_FORCEINLINE inline __attribute__((always_inline))
#define PS2_NOINLINE __attribute__((noinline))
#define PS2_LIKELY(x) __builtin_expect(!!(x), 1)
#endif

#endif // PS2_INLINE_H
