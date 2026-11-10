#include <metal_stdlib>
#include <metal_texture>
using namespace metal;
struct Globals_Type
{
    uint count;
    float3 target;
    float radius;
    float4 colors[2];
};

struct items_Type
{
    uint value[4];
};

struct counts_Type
{
    uint value[1];
};

struct TGSM0_Type
{
    uint value[1];
};

constant float4 ImmCB_0[2] =
{
	float4(1.0, 0.0, 0.0, 0.0),
	float4(0.0, 1.0, 0.0, 0.0)
};
kernel void computeMain(
    constant Globals_Type& Globals [[ buffer(1) ]],
    const device items_Type *items [[ buffer(2) ]],
    const device uint *raw [[ buffer(3) ]],
    device counts_Type *counts [[ buffer(0) ]],
    uint3 mtl_ThreadID [[ thread_position_in_grid ]],
    uint3 mtl_ThreadIDInGroup [[ thread_position_in_threadgroup ]])
{
    float u_xlat0;
    int u_xlati1;
    uint u_xlatu2;
    threadgroup TGSM0_Type TGSM0[64];
    TGSM0[mtl_ThreadIDInGroup.x].value[(0x0 >> 2)] = items[mtl_ThreadID.x].value[(0x0 >> 2) + 0];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    u_xlat0 = fma(as_type<float>(items[mtl_ThreadID.x].value[(0x4 >> 2) + 0]), Globals.radius, Globals.target.y);
    u_xlati1 = atomic_fetch_add_explicit(reinterpret_cast<device atomic_int *>(&counts[0x0].value[(0x0 >> 2)]), 0x1, memory_order::memory_order_relaxed);
    u_xlat0 = u_xlat0 + ImmCB_0[u_xlati1 & 0x1].y;
    u_xlatu2 = raw[mtl_ThreadID.x] + TGSM0[0x0].value[(0x0 >> 2)] + uint(u_xlati1);
    while(true){
        {
            uint compare_value = u_xlatu2;
            atomic_compare_exchange_weak_explicit(reinterpret_cast<device atomic_uint *>(&counts[0x1].value[(0x0 >> 2)]), &compare_value, as_type<uint>(u_xlat0), memory_order::memory_order_relaxed, memory_order::memory_order_relaxed);
            u_xlatu2 = compare_value;
        }
        if((u_xlatu2==0x0u)){break;}
    }
    return;
}
