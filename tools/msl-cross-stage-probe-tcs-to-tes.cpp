// Probe: replicate AppGL-W's planned `siblingTcsOutputSpirv` direction.
// Walks TCS post-compile main0_out, adds non-builtin members as fake
// inputs to TES via add_msl_shader_input, then introspects TES main0_in
// to determine whether synth-fakes land BEFORE or AFTER natural inputs
// in the resulting struct order.
//
// Usage: msl-cross-stage-probe-tcs-to-tes <tcs.spv> <tes.spv>

#include "spirv_msl.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_set>
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

	// Step 1: introspect TCS post-compile to get its main0_out layout.
	CompilerMSL tcs_c(tcs.data(), tcs.size());
	{
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.multi_patch_workgroup = true;
		tcs_c.set_msl_options(o);
		(void)tcs_c.compile();
	}
	auto tcs_layout = tcs_c.get_msl_interface_layout(StorageClassOutput, false);
	printf("# TCS main0_out: %zu members, struct_size=%u\n",
	       tcs_layout.members.size(), tcs_layout.struct_size);
	for (auto &m : tcs_layout.members) {
		printf("  - name=%s off=%u sz=%u builtin=%d vecsize=%u\n",
		       m.name.c_str(), m.offset, m.size, m.is_builtin, m.vecsize);
	}

	// Step 2: get TES natural input names.
	CompilerMSL tes_c(tes.data(), tes.size());
	{
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.raw_buffer_tese_input = true;
		o.tess_evaluation_as_compute = true;
		o.capture_output_to_buffer = true;
		tes_c.set_msl_options(o);
	}
	auto tes_inputs_natural = tes_c.get_shader_resources().stage_inputs;
	std::unordered_set<std::string> tes_natural_names;
	printf("\n# TES natural inputs (pre-wiring): %zu\n", tes_inputs_natural.size());
	for (auto &r : tes_inputs_natural) {
		tes_natural_names.insert(r.name);
		printf("  - name=%s\n", r.name.c_str());
	}

	// Step 3: name-match dedup, add unmatched TCS outputs as fakes to TES.
	uint32_t fake_loc = 1000;
	for (auto &m : tcs_layout.members) {
		if (m.is_builtin) continue;
		if (tes_natural_names.count(m.name)) {
			printf("# skip TCS '%s' — TES has it natively\n", m.name.c_str());
			continue;
		}
		MSLShaderInterfaceVariable v;
		v.location = fake_loc++;
		v.component = 0;
		v.format = MSL_SHADER_VARIABLE_FORMAT_ANY32;
		v.builtin = BuiltInMax;
		v.vecsize = m.vecsize;
		v.rate = MSL_SHADER_VARIABLE_RATE_PER_VERTEX;
		tes_c.add_msl_shader_input(v);
		printf("# add fake input for TCS '%s' at fake-loc %u\n", m.name.c_str(), v.location - 1);
	}

	// Step 4: compile TES, introspect main0_in to check struct order.
	(void)tes_c.compile();
	auto tes_layout = tes_c.get_msl_interface_layout(StorageClassInput, false);
	printf("\n# TES main0_in POST-WIRING: %zu members, struct_size=%u\n",
	       tes_layout.members.size(), tes_layout.struct_size);
	for (size_t i = 0; i < tes_layout.members.size(); ++i) {
		auto &m = tes_layout.members[i];
		printf("  [%zu] name=%-30s off=%u sz=%u builtin=%d loc=%s vecsize=%u\n",
		       i, m.name.c_str(), m.offset, m.size, m.is_builtin,
		       m.location == ~0u ? "-" : std::to_string(m.location).c_str(),
		       m.vecsize);
	}

	// Step 5: structural verdict.
	printf("\n# === VERDICT ===\n");
	if (tes_layout.members.empty()) {
		printf("# EMPTY — TES has no inputs at all.\n");
	} else {
		bool fake_before_natural = false;
		bool natural_before_fake = false;
		bool seen_natural = false;
		for (auto &m : tes_layout.members) {
			bool is_natural = tes_natural_names.count(m.name) > 0;
			if (is_natural) seen_natural = true;
			else if (seen_natural) natural_before_fake = true;
			else fake_before_natural = true;
		}
		if (fake_before_natural && !natural_before_fake)
			printf("# FAKES BEFORE NATURAL — Option A's struct order works.\n");
		else if (natural_before_fake && !fake_before_natural)
			printf("# NATURAL BEFORE FAKES — Option A struct-order is WRONG; need C/B.\n");
		else if (fake_before_natural && natural_before_fake)
			printf("# MIXED — fakes interleave with naturals; condition-dependent.\n");
		else
			printf("# ALL-SAME-CATEGORY — only one type of member present, order moot.\n");
	}

	return 0;
}
