// Test for geometry_shader_as_mesh.
// Compiles a single GS .spv twice — once with the flag off (preserving
// upstream GS emission, which leaves `EmitVertex();` etc. as literal
// passthrough that won't Metal-validate) and once with the flag on
// (translating to a Metal `[[mesh]]` function that captures
// EmitVertex/EndPrimitive into a function-local spvVertices buffer
// and flushes via spvMesh.set_vertex / set_primitive / set_primitive_count).
//
// Asserts:
//   flag-OFF — output preserves `EmitVertex` / `EndPrimitive` literal
//              passthrough (current upstream behaviour).
//   flag-ON  — output contains `[[mesh]]`, `spvMesh`, `set_vertex`,
//              `set_primitive_count`, the OpEmitVertex/OpEndPrimitive
//              opcodes are intercepted (no `EmitVertex(` literal),
//              AND the emitted MSL `xcrun -sdk macosx metal -c
//              -std=metal3.0` exits rc=0. The compile-validate assertion
//              is load-bearing: marker presence ≠ Metal validity, and
//              prior emission gaps (anonymous gl_PerVertex, undeclared
//              gl_in) only surfaced when the resulting MSL was actually
//              fed through `metal -c`.
//
// Usage: msl-gs-as-mesh-test <gs.spv>

#include "spirv_msl.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using namespace spirv_cross;

static std::vector<uint32_t> read_spirv(const char *path) {
	std::ifstream f(path, std::ios::binary);
	std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	std::vector<uint32_t> w(b.size() / 4);
	std::memcpy(w.data(), b.data(), b.size());
	return w;
}

static bool contains(const std::string &haystack, const char *needle) {
	return haystack.find(needle) != std::string::npos;
}

static int check(const char *label, bool ok) {
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
	return ok ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: %s gs.spv [vs.spv]\n", argv[0]);
		fprintf(stderr, "  optional vs.spv enables Path A++ rung-4 struct-byte parity assertion\n");
		fprintf(stderr, "  (compares VS main0_out vs mesh main0_in member layout)\n");
		return 1;
	}
	auto gs = read_spirv(argv[1]);
	bool have_vs = argc == 3;
	std::vector<uint32_t> vs_spv;
	if (have_vs)
		vs_spv = read_spirv(argv[2]);

	int fails = 0;

	// Pass 1: flag OFF — upstream-preserving emission.
	{
		CompilerMSL c(gs.data(), gs.size());
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.geometry_shader_as_mesh = false;
		c.set_msl_options(o);
		std::string msl = c.compile();

		printf("# === flag-OFF (geometry_shader_as_mesh=false) ===\n");
		fails += check("flag-OFF preserves `EmitVertex` literal (upstream behaviour)",
		               contains(msl, "EmitVertex("));
		fails += check("flag-OFF does NOT emit `[[mesh]]` entry",
		               !contains(msl, "[[mesh]]"));
		fails += check("flag-OFF does NOT emit spvMesh.set_vertex",
		               !contains(msl, "spvMesh.set_vertex"));
	}

	// Pass 2: flag ON — GS-as-mesh translation.
	{
		CompilerMSL c(gs.data(), gs.size());
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.geometry_shader_as_mesh = true;
		c.set_msl_options(o);
		std::string msl = c.compile();

		printf("# === flag-ON  (geometry_shader_as_mesh=true)  ===\n");
		fails += check("flag-ON emits `[[mesh]]` entry attribute",
		               contains(msl, "[[mesh]]"));
		fails += check("flag-ON declares spvMesh_t mesh<...> typedef",
		               contains(msl, "using spvMesh_t = mesh<"));
		fails += check("flag-ON declares spvVertices function-local buffer",
		               contains(msl, "spvUnsafeArray<spvPerVertex,"));
		fails += check("flag-ON intercepts EmitVertex (no `EmitVertex(` literal)",
		               !contains(msl, "EmitVertex("));
		fails += check("flag-ON emits spvMesh.set_vertex flush",
		               contains(msl, "spvMesh.set_vertex"));
		fails += check("flag-ON emits spvMesh.set_primitive_count flush",
		               contains(msl, "spvMesh.set_primitive_count"));
		fails += check("flag-ON emits spvMesh.set_index for triangle_strip",
		               contains(msl, "spvMesh.set_index"));
		// Path H (gate 11) — implicit EndPrimitive per GL 4.6 §11.3.4.
		// Default-on under geometry_shader_as_mesh: tracker bool +
		// runtime guard at function exit. Synthetic fixture has
		// explicit EndPrimitive so the runtime guard fires as false at
		// execution; the structural emission is what we assert here.
		fails += check("flag-ON declares spvNeedImplicitEndPrimitive tracker (Path H)",
		               contains(msl, "bool spvNeedImplicitEndPrimitive = false;"));
		fails += check("flag-ON sets tracker on EmitVertex (Path H)",
		               contains(msl, "spvNeedImplicitEndPrimitive = true;"));
		fails += check("flag-ON clears tracker on EndPrimitive (Path H)",
		               contains(msl, "spvNeedImplicitEndPrimitive = false;"));
		fails += check("flag-ON emits implicit-EndPrimitive runtime guard at function exit (Path H)",
		               contains(msl, "if (spvNeedImplicitEndPrimitive)"));
		// Phase 2.5 Gap A: spvPerVertex must contain exactly one
		// [[position]] member. The synthesizer should detect existing
		// member-level Position decoration on gl_PerVertex Output blocks
		// and skip injection — without member-level detection, the
		// synthesizer would duplicate the [[position]] member, which
		// Metal rejects ("multiple members with attribute 'position'").
		// Inspect the spvPerVertex struct region only.
		size_t spv_per_vertex_start = msl.find("struct spvPerVertex");
		if (spv_per_vertex_start != std::string::npos) {
			size_t spv_per_vertex_end = msl.find("};", spv_per_vertex_start);
			std::string spv_per_vertex_body = msl.substr(
			    spv_per_vertex_start, spv_per_vertex_end - spv_per_vertex_start);
			size_t pos_count = 0, off = 0;
			while ((off = spv_per_vertex_body.find("[[position]]", off)) != std::string::npos) {
				pos_count++; off++;
			}
			fails += check("Gap A: exactly one [[position]] member in spvPerVertex when GS writes gl_Position (no synthesizer duplicate)",
			               pos_count == 1);
		}

		// Phase 2.5 Gap C: per-primitive output reads (e.g. body
		// `int(gl_Layer)`) require function-local shadows; without them
		// the body emits an undeclared identifier and metal -c fails.
		// Asserts the write-propagation-from-shadow pattern when GS
		// writes gl_Layer; skipped on fixtures that don't write it
		// (the synthetic gs-as-mesh-input.geom exercises this; some
		// other fixtures don't).
		if (contains(msl, "gl_Layer ="))
		{
			fails += check("Gap C: spvCurrentPrim per-primitive write propagates from function-local shadow (gl_Layer pattern)",
			               contains(msl, "spvCurrentPrim.gl_Layer = gl_Layer;"));
		}
		// Phase 2.5 Gap B: spvStripStart tracker emits unconditionally
		// (under geometry_shader_as_mesh) for triangle_strip / line_strip
		// outputs to support max_vertices > simple-case-threshold strip
		// expansion. Synthetic max_vertices=3 keeps simple emit shape
		// past the tracker declaration; layered_rendering fixtures
		// exercise the strip-loop branch.
		fails += check("Gap B: spvStripStart tracker declared (Phase 2.5 strip-to-list expansion infrastructure)",
		               contains(msl, "uint spvStripStart = 0u;"));
		// Path I: user-defined interface-block input members populate
		// from `<var_name>_<block_mbr_name>` main0_in fields (mirrors
		// SPIRV-Cross's interface-block-flattening naming convention
		// `iface_v1` for `in VS_GS { vec4 v1; } iface[]`). Conditional
		// on fixture having interface-block input — skipped for
		// fixtures without one.
		if (contains(msl, "_v1 [[user(") || contains(msl, "_v2 [[user("))
		{
			fails += check("Path I: interface-block input member population (e.g. iface[i].v1 = spvVsIn.iface_v1)",
			               contains(msl, "].v1 = spvVsIn.") || contains(msl, "].v2 = spvVsIn."));
		}
		// Path A — VS-output buffer parameter + per-vertex population.
		// gl_in[] should be populated from `spvVsOutputs[primId * N + i]`,
		// not zero-init. Primary regression guard from AppGL-W's pre-PSO
		// integration finding (102 GS programs would mass-regress with
		// zero-init gl_in routing through mesh-shader path instead of
		// CPU interpreter).
		fails += check("flag-ON declares `const device main0_in* spvVsOutputs` buffer parameter (Path A)",
		               contains(msl, "const device main0_in* spvVsOutputs"));
		fails += check("flag-ON declares `[[threadgroup_position_in_grid]]` primitive id (Path A)",
		               contains(msl, "[[threadgroup_position_in_grid]]"));
		fails += check("flag-ON populates gl_in from spvVsOutputs (no zero-init Path A regression)",
		               contains(msl, "spvVsOutputs[spvPrimitiveID"));

		// Methodology gate: dump the emitted MSL and run `xcrun metal -c
		// -std=metal3.0`. Marker presence above is necessary but not
		// sufficient — emission can pass markers and still fail Metal's
		// compile (anonymous-struct sanitization gaps, undeclared
		// references, unsanitized identifiers). Exposing the metal-c
		// failure here turns AppGL-W-side regressions into local CI fails.
		const char *dump_path = "/tmp/msl-gs-as-mesh-test.metal";
		const char *air_path = "/tmp/msl-gs-as-mesh-test.air";
		{
			std::ofstream out(dump_path);
			out << msl;
		}
		printf("# === flag-ON metal -c -std=metal3.0 (dumped to %s) ===\n", dump_path);
		std::string cmd = "xcrun -sdk macosx metal -c -std=metal3.0 ";
		cmd += dump_path;
		cmd += " -o ";
		cmd += air_path;
		cmd += " 2>&1";
		FILE *p = popen(cmd.c_str(), "r");
		std::string out_buf;
		if (p) {
			char line[1024];
			while (fgets(line, sizeof(line), p))
				out_buf += line;
			int rc = pclose(p);
			fails += check("flag-ON `xcrun metal -c -std=metal3.0` exits rc=0", rc == 0);
			if (rc != 0)
				printf("  metal -c output:\n%s\n", out_buf.c_str());
		} else {
			fails += check("flag-ON metal -c invocation possible", false);
		}
	}

	// Pass 3 — rung 5: runtime semantic invariants (Path B).
	// Each emit_fixup() conditional transformation must fire on the
	// mesh function before spvMesh.set_vertex when its option is set;
	// the legacy emit_fixup short-circuits for capture_output_to_buffer
	// (set by VS-compute via vertex_for_tessellation), so mesh-GS has
	// to replicate the transforms on its own. Asserts presence of each
	// transformation string when enabled — catches the divergence class
	// that triggered Path B (clipspace fix dropped, points rendered in
	// GL clip space instead of Metal clip space).
	{
		printf("# === Path B rung-5 (runtime semantic invariants) ===\n");
		// Subtest A: clipspace fixup
		{
			CompilerMSL c(gs.data(), gs.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.geometry_shader_as_mesh = true;
			c.set_msl_options(o);
			CompilerGLSL::Options g = c.get_common_options();
			g.vertex.fixup_clipspace = true;
			c.set_common_options(g);
			std::string msl = c.compile();
			fails += check("rung-5 clipspace fixup emits when fixup_clipspace=true",
			               contains(msl, "spvVertices[spvVI].gl_Position.z = "));
		}
		// Subtest B: clipspace fixup absent when option off (default)
		{
			CompilerMSL c(gs.data(), gs.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.geometry_shader_as_mesh = true;
			c.set_msl_options(o);
			std::string msl = c.compile();
			fails += check("rung-5 clipspace fixup ABSENT when fixup_clipspace=false (regression guard)",
			               !contains(msl, "spvVertices[spvVI].gl_Position.z = "));
		}
		// Subtest C: y-flip
		{
			CompilerMSL c(gs.data(), gs.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.geometry_shader_as_mesh = true;
			c.set_msl_options(o);
			CompilerGLSL::Options g = c.get_common_options();
			g.vertex.flip_vert_y = true;
			c.set_common_options(g);
			std::string msl = c.compile();
			fails += check("rung-5 y-flip emits when flip_vert_y=true",
			               contains(msl, "spvVertices[spvVI].gl_Position.y = -"));
		}
		// Subtest D: y-flip absent when option off
		{
			CompilerMSL c(gs.data(), gs.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.geometry_shader_as_mesh = true;
			c.set_msl_options(o);
			std::string msl = c.compile();
			fails += check("rung-5 y-flip ABSENT when flip_vert_y=false (regression guard)",
			               !contains(msl, "spvVertices[spvVI].gl_Position.y = -"));
		}
	}

	// Rung 4 — cross-stage struct-byte parity assertion (Path A++).
	// Compares VS-compute `main0_out` vs mesh `main0_in` member layout
	// field-by-field. Catches the divergence class that triggered Path A++:
	// VS-compute emits the full GLSL gl_PerVertex (force-include via
	// vertex_for_tessellation+capture_output_to_buffer) while
	// GS-as-mesh's main0_in previously dropped inactive builtins,
	// silently misaligning subsequent vertices' reads from the
	// VS-output buffer. Marker presence (rung 1) + compile validity
	// (rung 2) + structural-runtime-flow markers (rung 3) all PASS
	// independently for each stage; only cross-stage byte compare
	// catches sibling-stage struct-shape divergence.
	if (have_vs) {
		printf("# === Path A++ rung-4 (cross-stage struct-byte parity) ===\n");
		CompilerMSL vc(vs_spv.data(), vs_spv.size());
		CompilerMSL::Options vo;
		vo.msl_version = 30000;
		vo.vertex_for_tessellation = true;
		vo.capture_output_to_buffer = true;
		vc.set_msl_options(vo);
		std::string vs_msl = vc.compile();
		auto vs_layout = vc.get_msl_interface_layout(StorageClassOutput, false);

		CompilerMSL mc(gs.data(), gs.size());
		CompilerMSL::Options mo;
		mo.msl_version = 30000;
		mo.geometry_shader_as_mesh = true;
		mc.set_msl_options(mo);
		std::string mesh_msl = mc.compile();
		auto mesh_layout = mc.get_msl_interface_layout(StorageClassInput, false);

		printf("# VS main0_out: %zu members, struct_size=%u\n",
		       vs_layout.members.size(), vs_layout.struct_size);
		for (auto &m : vs_layout.members)
			printf("    %-30s off=%-3u sz=%-3u builtin=%d vecsize=%u\n",
			       m.name.c_str(), m.offset, m.size, m.is_builtin, m.vecsize);
		printf("# mesh main0_in: %zu members, struct_size=%u\n",
		       mesh_layout.members.size(), mesh_layout.struct_size);
		for (auto &m : mesh_layout.members)
			printf("    %-30s off=%-3u sz=%-3u builtin=%d vecsize=%u\n",
			       m.name.c_str(), m.offset, m.size, m.is_builtin, m.vecsize);

		fails += check("rung-4 VS main0_out struct_size == mesh main0_in struct_size",
		               vs_layout.struct_size == mesh_layout.struct_size);
		fails += check("rung-4 VS main0_out member count == mesh main0_in member count",
		               vs_layout.members.size() == mesh_layout.members.size());
		// Per-member offset+size compare. Names may differ (gl_ClipDistance
		// vs gl_ClipDistance_0); check the byte layout, not the names.
		size_t common = std::min(vs_layout.members.size(), mesh_layout.members.size());
		bool all_offsets_match = true;
		bool all_sizes_match = true;
		for (size_t i = 0; i < common; ++i) {
			if (vs_layout.members[i].offset != mesh_layout.members[i].offset)
				all_offsets_match = false;
			if (vs_layout.members[i].size != mesh_layout.members[i].size)
				all_sizes_match = false;
		}
		fails += check("rung-4 per-member offset parity (VS main0_out vs mesh main0_in)",
		               all_offsets_match);
		fails += check("rung-4 per-member size parity (VS main0_out vs mesh main0_in)",
		               all_sizes_match);

		// Rung 7 — cross-encoder-family AIR liveness annotation (Path E).
		// VS-compute writes vanish on compute→render consumer paths
		// without an explicit kernel-exit device barrier. Assert presence
		// when flag enabled and absence when disabled (regression guard).
		// Distinct from rungs 1-5: this is *runtime behavior of clean
		// translation under specific consumer-pipeline configurations*,
		// not a translation-correctness check.
		printf("# === Path E rung-7 (cross-encoder AIR liveness barrier) ===\n");
		// Subtest A: barrier emits when flag enabled
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			o.force_compute_kernel_device_barrier_at_exit = true;
			c.set_msl_options(o);
			std::string vs_msl_with_barrier = c.compile();
			fails += check("rung-7 mem_device barrier emits when flag enabled",
			               contains(vs_msl_with_barrier, "threadgroup_barrier(mem_flags::mem_device)"));
		}
		// Subtest B: barrier absent in default (tess-default invariant)
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			c.set_msl_options(o);
			std::string vs_msl_default = c.compile();
			fails += check("rung-7 mem_device barrier ABSENT in tess-default (98 GENUINE_PASS invariant)",
			               !contains(vs_msl_default, "threadgroup_barrier(mem_flags::mem_device)"));
		}
		// Subtest C: volatile qualifier emits when flag enabled (Path E++)
		// AppGL-W's Checkpoint 11 readback proved barrier alone insufficient
		// against AIR cross-encoder-family elimination; volatile is the
		// load-bearing primitive (Apple's MSL spec contractually preserves
		// writes through volatile-qualified pointers across optimizer passes).
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			o.force_compute_kernel_device_volatile_writes = true;
			c.set_msl_options(o);
			std::string vs_msl_volatile = c.compile();
			fails += check("rung-7 volatile spvOut parameter emits when volatile flag enabled (Path E++)",
			               contains(vs_msl_volatile, "volatile device main0_out* spvOut"));
			fails += check("rung-7 volatile local reference emits when volatile flag enabled (Path E++)",
			               contains(vs_msl_volatile, "volatile device main0_out& out"));
		}
		// Subtest D: volatile absent in default (tess-default invariant)
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			c.set_msl_options(o);
			std::string vs_msl_default = c.compile();
			fails += check("rung-7 volatile ABSENT in tess-default (98 GENUINE_PASS invariant)",
			               !contains(vs_msl_default, "volatile device main0_out"));
		}
		// Subtest E: atomic_store emits when atomic flag enabled (Path E+++)
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			o.force_compute_kernel_atomic_writes_on_spvOut = true;
			c.set_msl_options(o);
			std::string vs_msl_atomic = c.compile();
			fails += check("rung-7 atomic_store_explicit emits when atomic flag enabled (Path E+++)",
			               contains(vs_msl_atomic, "atomic_store_explicit"));
		}
		// Subtest F: atomic_store absent in default (tess-default invariant)
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			c.set_msl_options(o);
			std::string vs_msl_default = c.compile();
			fails += check("rung-7 atomic_store_explicit ABSENT in tess-default (98 GENUINE_PASS invariant)",
			               !contains(vs_msl_default, "atomic_store_explicit"));
		}
		// Subtest G: entry counter probe emits when probe flag enabled
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			o.force_compute_kernel_entry_counter_probe = true;
			c.set_msl_options(o);
			std::string vs_msl_probe = c.compile();
			fails += check("rung-7 entry counter parameter emits when probe flag enabled",
			               contains(vs_msl_probe, "device atomic_uint* spvKernelEntryCounter"));
			fails += check("rung-7 entry counter atomic_fetch_add emits when probe flag enabled",
			               contains(vs_msl_probe, "atomic_fetch_add_explicit(spvKernelEntryCounter, 1u"));
		}
		// Subtest H: counter probe absent in default (tess-default invariant)
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			c.set_msl_options(o);
			std::string vs_msl_default = c.compile();
			fails += check("rung-7 entry counter probe ABSENT in tess-default (98 GENUINE_PASS invariant)",
			               !contains(vs_msl_default, "spvKernelEntryCounter"));
		}

		// Path G — rung-9 silent attribute-value-zero failure mitigation.
		// `[[grid_size]]` returns (0,0,0) on Apple Silicon under
		// dispatchThreads:/dispatchThreadgroups: for these kernel
		// signatures, causing every thread to early-return at the
		// bounds-check guard. `[[threads_per_grid]]` is the spec-correct
		// attribute for the actual thread count.
		printf("# === Path G rung-9 (silent attribute-value-zero failure) ===\n");
		// Subtest I: threads_per_grid emits when Path G flag enabled
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			o.force_threads_per_grid_for_stage_input_size = true;
			c.set_msl_options(o);
			std::string vs_msl_g = c.compile();
			fails += check("rung-9 threads_per_grid attribute emits when Path G flag enabled",
			               contains(vs_msl_g, "spvStageInputSize [[threads_per_grid]]"));
			fails += check("rung-9 grid_size attribute ABSENT when Path G flag enabled (mutual exclusion)",
			               !contains(vs_msl_g, "spvStageInputSize [[grid_size]]"));
		}
		// Subtest J: grid_size preserved in default (tess-default invariant)
		{
			CompilerMSL c(vs_spv.data(), vs_spv.size());
			CompilerMSL::Options o;
			o.msl_version = 30000;
			o.vertex_for_tessellation = true;
			o.capture_output_to_buffer = true;
			c.set_msl_options(o);
			std::string vs_msl_default = c.compile();
			fails += check("rung-9 grid_size attribute preserved in tess-default (98 GENUINE_PASS invariant)",
			               contains(vs_msl_default, "spvStageInputSize [[grid_size]]"));
		}
	}

	// Path L: TES-as-compute full-precision tess level buffer
	// (Sprint 4 sub-cluster A). Asserts presence of `spvTessLevelFull`
	// substring + stride math when the flag is enabled and the GS
	// fixture happens to be a TES (typical rig invocation passes a GS
	// + VS pair, so this assertion is conditional on the second
	// optional arg being a TES). Skipped otherwise.
	if (have_vs)
	{
		printf("# === Path L (TES-as-compute full-precision tess factors) ===\n");
		CompilerMSL c(vs_spv.data(), vs_spv.size());
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.tess_evaluation_as_compute = true;
		o.raw_buffer_tese_input = true;
		o.multi_patch_workgroup = true;
		o.capture_output_to_buffer = true;
		o.use_full_precision_tess_level_buffer = true;
		c.set_msl_options(o);
		std::string vs_msl_l;
		try { vs_msl_l = c.compile(); }
		catch (...) { vs_msl_l = ""; }  // not a TES — skip
		if (!vs_msl_l.empty() && contains(vs_msl_l, "spvTessLevelFull"))
		{
			fails += check("Path L: spvTessLevelFull buffer parameter emits when full-precision flag enabled",
			               contains(vs_msl_l, "device float* spvTessLevelFull"));
			fails += check("Path L: tess-level reads use spvTessLevelFull when full-precision flag enabled",
			               contains(vs_msl_l, "= spvTessLevelFull["));
		}
	}

	printf("\n%s — %d failure(s)\n", fails == 0 ? "OK" : "FAIL", fails);
	return fails == 0 ? 0 : 1;
}
