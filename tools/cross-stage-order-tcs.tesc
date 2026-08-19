#version 450
//
// CKPT34 mock TCS — emits out_uint at IR-ID position 0 (declared first),
// out_struct at IR-ID position 1 (declared second). Mirrors the
// "TCS out_uint(5)/out_struct(8)" half of the field-order mismatch
// surfaced by AppGL-W's β orchestrator on tc2te.gl_in.
//
layout(vertices = 4) out;

out gl_PerVertex { vec4 gl_Position; } gl_out[];

// Two scalar/vec outputs declared in this order (out_uint first, vec3 second).
// Mirrors the "out_uint(5)/out_struct(8)" byte sequence from CKPT34 minus
// the nested-struct complication that get_msl_interface_layout rejects in
// stage interface blocks.
layout(location = 0) out uint out_uint[];
layout(location = 1) out vec3 out_vec3[];

void main() {
    gl_out[gl_InvocationID].gl_Position = vec4(0.0);
    out_uint[gl_InvocationID] = 42u;
    out_vec3[gl_InvocationID] = vec3(1.0, 2.0, 3.0);
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = 1.0;
        gl_TessLevelOuter[1] = 1.0;
        gl_TessLevelOuter[2] = 1.0;
        gl_TessLevelOuter[3] = 1.0;
        gl_TessLevelInner[0] = 1.0;
        gl_TessLevelInner[1] = 1.0;
    }
}
