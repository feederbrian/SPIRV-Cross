// One-off test: replicate AppGL-W's β cross-stage wiring to investigate
// whether get_msl_interface_layout exposes synth-fake members from
// add_msl_shader_output.
//
// Usage: msl-cross-stage-probe <tcs.spv> <tes.spv>

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

int main(int argc, char **argv) {
	if (argc != 3) { fprintf(stderr, "usage: %s tcs.spv tes.spv\n", argv[0]); return 1; }

	auto tcs = read_spirv(argv[1]);
	auto tes = read_spirv(argv[2]);

	// Step 1: walk TES's inputs via reflection, mimicking what β probably does.
	CompilerMSL tes_c(tes.data(), tes.size());
	auto tes_inputs = tes_c.get_shader_resources().stage_inputs;
	printf("# TES has %zu stage_inputs:\n", tes_inputs.size());
	for (auto &r : tes_inputs) {
		uint32_t loc = tes_c.get_decoration(r.id, DecorationLocation);
		auto &t = tes_c.get_type(r.type_id);
		printf("  - name=%s id=%u loc=%u basetype=%d vecsize=%u\n",
		       r.name.c_str(), r.id, loc, (int)t.basetype, t.vecsize);
	}

	// Step 2: compile TCS with multi_patch_workgroup and call
	// add_msl_shader_output for each TES input — replicating β.
	CompilerMSL tcs_c(tcs.data(), tcs.size());
	CompilerMSL::Options o;
	o.msl_version = 30000;
	o.multi_patch_workgroup = true;
	tcs_c.set_msl_options(o);

	for (auto &r : tes_inputs) {
		MSLShaderInterfaceVariable v;
		v.location = tes_c.get_decoration(r.id, DecorationLocation);
		auto &t = tes_c.get_type(r.type_id);
		v.vecsize = t.vecsize;
		v.format = MSL_SHADER_VARIABLE_FORMAT_ANY32;
		v.rate = MSL_SHADER_VARIABLE_RATE_PER_VERTEX;
		tcs_c.add_msl_shader_output(v);
		printf("# wired TES input loc=%u vecsize=%u as TCS output\n", v.location, v.vecsize);
	}

	// Step 3: compile, then introspect.
	tcs_c.compile();

	auto layout = tcs_c.get_msl_interface_layout(StorageClassOutput, false);
	printf("# TCS main0_out post-compile: %zu members, struct_size=%u, align=%u\n",
	       layout.members.size(), layout.struct_size, layout.struct_alignment);
	for (auto &m : layout.members) {
		printf("  [%-30s] off=%u sz=%u  builtin=%s  loc=%s  base=%d vecsize=%u\n",
		       m.name.c_str(), m.offset, m.size,
		       m.is_builtin ? std::to_string((int)m.builtin).c_str() : "-",
		       m.location == ~0u ? "-" : std::to_string(m.location).c_str(),
		       (int)m.base_type, m.vecsize);
	}
	return 0;
}
