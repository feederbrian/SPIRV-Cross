// Hand-authored target MSL for the synthetic GS test.
// Demonstrates Path A: VS-output buffer parameter + per-vertex
// population of `gl_in[]` from `spvVsOutputs[primId * verticesPerPrim + i]`.
// Mirrors the actual SPIRV-Cross emission pattern for the synthetic
// `triangles in / triangle_strip out, max_vertices=3` GS.
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    constexpr const thread T& operator [] (size_t pos) const thread { return elements[pos]; }
    device T& operator [] (size_t pos) device { return elements[pos]; }
    constexpr const device T& operator [] (size_t pos) const device { return elements[pos]; }
    constexpr const constant T& operator [] (size_t pos) const constant { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup { return elements[pos]; }
};

struct gl_PerVertex_1 {
    float4 gl_Position [[position]];
};

struct spvPerVertex {
    float4 gl_Position [[position]];
    float4 out_color [[user(locn0)]];
};

struct spvPerPrimitive {
    uint gl_Layer [[render_target_array_index]];
};

using spvMesh_t = mesh<spvPerVertex, spvPerPrimitive, 3, 1, topology::triangle>;

// Path A: per-vertex VS-output struct. AppGL-W's runtime captures linked
// VS output (one main0_in per VS invocation) into `spvVsOutputs[buffer(22)]`
// and dispatches one mesh threadgroup per input primitive.
struct main0_in {
    float4 in_color [[user(locn0)]];
    float4 gl_Position [[position]];
};

[[mesh]] void main0(uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]],
                    const device main0_in* spvVsOutputs [[buffer(22)]],
                    uint spvPrimitiveID [[threadgroup_position_in_grid]],
                    spvMesh_t spvMesh)
{
    uint spvVertexIndex = 0u;
    uint spvPrimitiveIndex = 0u;
    spvUnsafeArray<spvPerVertex, 3> spvVertices = {};
    spvPerPrimitive spvCurrentPrim = {};
    spvUnsafeArray<gl_PerVertex_1, 3> gl_in;
    spvUnsafeArray<float4, 3> in_color;

    // Path A population — copy each per-vertex input from the VS-output buffer.
    for (uint spvVI = 0u; spvVI < 3u; ++spvVI) {
        const device main0_in& spvVsIn = spvVsOutputs[spvPrimitiveID * 3u + spvVI];
        gl_in[spvVI].gl_Position = spvVsIn.gl_Position;
        in_color[spvVI] = spvVsIn.in_color;
    }

    for (int i = 0; i < 3; ++i) {
        spvVertices[spvVertexIndex].gl_Position = gl_in[i].gl_Position;
        spvVertices[spvVertexIndex].out_color = in_color[i];
        spvCurrentPrim.gl_Layer = uint(i);
        ++spvVertexIndex;
    }
    // EndPrimitive: triangle indices + per-primitive output + counter increment
    spvMesh.set_index(spvPrimitiveIndex * 3u + 0u, 0u);
    spvMesh.set_index(spvPrimitiveIndex * 3u + 1u, 1u);
    spvMesh.set_index(spvPrimitiveIndex * 3u + 2u, 2u);
    spvMesh.set_primitive(spvPrimitiveIndex, spvCurrentPrim);
    ++spvPrimitiveIndex;

    // SetMeshOutputsEXT-equivalent: copy local arrays to spvMesh output
    spvMesh.set_primitive_count(spvPrimitiveIndex);
    for (uint vi = 0; vi < spvVertexIndex; ++vi) {
        spvMesh.set_vertex(vi, spvVertices[vi]);
    }
}
