#version 450
//
// CKPT34 mock TES — declares out_struct FIRST (lower IR-ID), out_uint
// SECOND (higher IR-ID). Mirrors the "TES out_struct(5)/out_uint(6)"
// half of the field-order mismatch.
//
// Without Path J' Option E.3, TES main0_in emits members in IR-ID
// order: { out_struct, out_uint } — disagreeing with TCS main0_out
// order { out_uint, out_struct } and breaking cross-stage byte
// alignment.
//
layout(quads, equal_spacing, cw) in;

// Reverse declaration order vs TCS — out_vec3 first (lower IR-ID),
// out_uint second (higher IR-ID). Without Path J' Option E.3, TES
// main0_in emits in TES-IR-ID order { out_vec3, out_uint }, breaking
// cross-stage byte alignment with TCS main0_out.
layout(location = 1) in vec3 out_vec3[];
layout(location = 0) in uint out_uint[];

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = vec4(out_vec3[0].xy, float(out_uint[0]), 1.0);
    vColor      = vec4(out_vec3[0], 0.0);
}
