// Path I target — GS with interface-block-typed input.
#version 450
#extension GL_EXT_geometry_shader : require

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 0) in VS_GS {
    vec4 v1;
    vec4 v2;
} iface[];

layout(location = 4) out vec4 out_v1;
layout(location = 5) out vec4 out_v2;

void main()
{
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        out_v1 = iface[i].v1;
        out_v2 = iface[i].v2;
        EmitVertex();
    }
    EndPrimitive();
}
