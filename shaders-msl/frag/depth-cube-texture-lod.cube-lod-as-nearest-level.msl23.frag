#version 450 core
#extension GL_EXT_texture_shadow_lod : require
layout(location = 0) out float o_color;
layout(location = 0) in vec4 v_texCoord;
layout(location = 1) in float v_lod;
layout(set = 0, binding = 0) uniform highp samplerCubeShadow u_sampler;

void main()
{
	o_color = textureLod(u_sampler, v_texCoord, v_lod);
}
