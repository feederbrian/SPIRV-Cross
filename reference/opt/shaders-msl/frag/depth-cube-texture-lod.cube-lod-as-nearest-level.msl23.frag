#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float o_color [[color(0)]];
};

struct main0_in
{
    float4 v_texCoord [[user(locn0)]];
    float v_lod [[user(locn1)]];
};

fragment main0_out main0(main0_in in [[stage_in]], depthcube<float> u_sampler [[texture(0)]], sampler u_samplerSmplr [[sampler(0)]])
{
    main0_out out = {};
    out.o_color = u_sampler.sample_compare(u_samplerSmplr, in.v_texCoord.xyz, in.v_texCoord.w, level(max(0.0, floor(in.v_lod + 0.5))));
    return out;
}

