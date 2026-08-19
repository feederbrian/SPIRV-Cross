// Gap A target — GS that doesn't write gl_Position (TF-only / depth-only pattern).
// Per GL 4.6 GS spec, gl_Position default is undefined when not written, but
// the program is well-formed. Mesh-pipeline emission must include gl_Position
// in spvPerVertex or Metal rejects the mesh<vertex_t,...> instantiation.
// Synthesize gl_Position Output in IR when GS doesn't write one.
#version 450
#extension GL_EXT_geometry_shader : require

layout(lines_adjacency)        in;
layout(line_strip, max_vertices = 2) out;

layout(location = 0) out vec4 out_adjacent_geometry;
layout(location = 1) out vec4 out_geometry;

void main()
{
    out_adjacent_geometry = gl_in[0].gl_Position;
    out_geometry          = gl_in[1].gl_Position;
    EmitVertex();
    out_adjacent_geometry = gl_in[3].gl_Position;
    out_geometry          = gl_in[2].gl_Position;
    EmitVertex();
    EndPrimitive();
}
