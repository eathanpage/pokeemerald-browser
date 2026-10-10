// Integer division helpers. WebAssembly traps on division by zero (and on
// INT_MIN / -1), while the GBA's division routines just return 0. The game
// divides by zero in a few places (e.g. the Spark animation), so after
// linking, port/tools/wasm_safe_div.py rewrites the division instructions
// into calls to these functions.

#include <emscripten.h>
#include <stdint.h>

EMSCRIPTEN_KEEPALIVE int32_t Port_DivS32(int32_t a, int32_t b)
{
    if (b == 0)
        return 0;
    if (b == -1)
        return (int32_t)(0u - (uint32_t)a);
    return a / b;
}

EMSCRIPTEN_KEEPALIVE uint32_t Port_DivU32(uint32_t a, uint32_t b)
{
    return b ? a / b : 0;
}

EMSCRIPTEN_KEEPALIVE int32_t Port_RemS32(int32_t a, int32_t b)
{
    if (b == 0 || b == -1)
        return 0;
    return a % b;
}

EMSCRIPTEN_KEEPALIVE uint32_t Port_RemU32(uint32_t a, uint32_t b)
{
    return b ? a % b : 0;
}

EMSCRIPTEN_KEEPALIVE int64_t Port_DivS64(int64_t a, int64_t b)
{
    if (b == 0)
        return 0;
    if (b == -1)
        return (int64_t)(0u - (uint64_t)a);
    return a / b;
}

EMSCRIPTEN_KEEPALIVE uint64_t Port_DivU64(uint64_t a, uint64_t b)
{
    return b ? a / b : 0;
}

EMSCRIPTEN_KEEPALIVE int64_t Port_RemS64(int64_t a, int64_t b)
{
    if (b == 0 || b == -1)
        return 0;
    return a % b;
}

EMSCRIPTEN_KEEPALIVE uint64_t Port_RemU64(uint64_t a, uint64_t b)
{
    return b ? a % b : 0;
}
