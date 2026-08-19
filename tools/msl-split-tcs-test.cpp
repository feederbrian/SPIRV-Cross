// Test for split_tcs_outputs_by_consumption.
// Replicates β: walk TES inputs, add_msl_shader_output them to TCS,
// then compile TCS with the new flag set. Inspect main0_out — should
// shrink to TES-consumed members only. Inspect emitted MSL — should
// see threadgroup-memory routing for masked outputs.
//
// Usage: msl-split-tcs-test <tcs.spv> <tes.spv>

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

	// Walk TES inputs.
	CompilerMSL tes_c(tes.data(), tes.size());
	auto tes_inputs = tes_c.get_shader_resources().stage_inputs;
	printf("# TES inputs (%zu):\n", tes_inputs.size());
	for (auto &r : tes_inputs)
		printf("  - name=%s loc=%u\n", r.name.c_str(),
		       tes_c.get_decoration(r.id, DecorationLocation));

	// Compile TCS twice — once WITHOUT the flag (baseline), once WITH.
	for (int with_flag = 0; with_flag < 2; ++with_flag) {
		CompilerMSL tcs_c(tcs.data(), tcs.size());
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.multi_patch_workgroup = true;
		o.split_tcs_outputs_by_consumption = (with_flag != 0);
		tcs_c.set_msl_options(o);

		// Wire β-style: each TES input becomes a TCS output via add_msl_shader_output.
		// Pass NAME as well — used by classify_tcs_outputs_by_consumption.
		for (auto &r : tes_inputs) {
			MSLShaderInterfaceVariable v;
			v.location = tes_c.get_decoration(r.id, DecorationLocation);
			v.component = 0;
			v.format = MSL_SHADER_VARIABLE_FORMAT_ANY32;
			v.builtin = BuiltInMax;
			v.vecsize = tes_c.get_type(r.type_id).vecsize;
			v.rate = MSL_SHADER_VARIABLE_RATE_PER_VERTEX;
			v.name = r.name;
			tcs_c.add_msl_shader_output(v);
		}

		std::string msl = tcs_c.compile();

		auto layout = tcs_c.get_msl_interface_layout(StorageClassOutput, false);
		printf("\n# === TCS main0_out (split_tcs_outputs_by_consumption=%d) ===\n", with_flag);
		printf("# %zu members, struct_size=%u\n", layout.members.size(), layout.struct_size);
		for (auto &m : layout.members) {
			printf("  - name=%-20s off=%u sz=%u builtin=%d vecsize=%u\n",
			       m.name.c_str(), m.offset, m.size, m.is_builtin, m.vecsize);
		}

		// Hunt for threadgroup-memory routing in the emitted MSL.
		size_t tg_count = 0, pos = 0;
		while ((pos = msl.find("threadgroup ", pos)) != std::string::npos) {
			tg_count++;
			pos++;
		}
		printf("# 'threadgroup ' occurrences in emitted MSL: %zu\n", tg_count);
		// Print the lines containing threadgroup for inspection.
		size_t line_start = 0;
		for (size_t i = 0; i < msl.size(); ++i) {
			if (msl[i] == '\n') {
				std::string line = msl.substr(line_start, i - line_start);
				if (line.find("threadgroup") != std::string::npos)
					printf("    %s\n", line.c_str());
				line_start = i + 1;
			}
		}
	}

	return 0;
}
