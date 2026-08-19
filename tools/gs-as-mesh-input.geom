// Synthetic GS for SPIRV-W Sprint 3 Step 2 — `geometry_shader_as_mesh` validation.
//
// Spec features exercised:
//   - Input topology:           triangles (3 input vertices per primitive)
//   - Output topology:          triangle_strip (3 output vertices = 1 triangle)
//   - max_vertices:             3
//   - EmitVertex calls:         3 (one per input vertex pass-through)
//   - EndPrimitive calls:       1 (closes the strip after the 3 emits)
//   - Per-primitive output:     gl_Layer (assigned per-vertex in source, captured per-primitive in mesh form)
//   - Streams:                  none (defer per Step 2 scope)
//   - Adjacency input:          none (defer per Step 2 scope)
//   - Output user varyings:     vec4 out_color (per-vertex)
//   - gl_in array reads:        gl_in[i].gl_Position
//
// This is the minimum-viable GS test for the "easy + medium" Step 2 scope:
// triangle in/out + EmitVertex/EndPrimitive + per-primitive gl_Layer.

#version 450
#extension GL_EXT_geometry_shader : require

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 0) in vec4 in_color[];
layout(location = 0) out vec4 out_color;

void main()
{
    for (int i = 0; i < gl_in.length(); ++i)
    {
        gl_Position = gl_in[i].gl_Position;
        out_color = in_color[i];
        gl_Layer = i;
        EmitVertex();
    }
    EndPrimitive();
}
