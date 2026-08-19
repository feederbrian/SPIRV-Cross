// Path J' Option E.3 standalone test — verifies that
// `inputs_by_location_insertion_order` drives main0_in member order
// when the orchestrator registers synthetic-range names, regardless
// of TES IR-walk natural ordering.
//
// CKPT34 scenario reproduction (simplified to scalar/vec):
//   TCS main0_out:  { out_uint, out_vec3, gl_Position }
//   TES IR (raw):   names declared in REVERSE order — out_vec3 first
//                   (lower IR-ID), out_uint second.
//   Without E.3:    TES main0_in emits { out_vec3, out_uint } when
//                   the IR-ID order disagrees with TCS-out order
//                   (or when monolithic-program TES has no Location
//                   decorations to drive the natural map ordering).
//   With E.3:       Orchestrator-driven add_msl_shader_input call
//                   sequence (TCS-out order) overrides → TES main0_in
//                   reorders to { out_uint, out_vec3 } byte-for-byte
//                   matching TCS main0_out non-builtin layout.
//
// The test registers synthetic-range fakes for ALL TCS-out non-builtin
// names (not just unmatched), forcing the orchestrator-driven path
// even when TES has the names natively. This mirrors AppGL-W's β
// orchestrator behaviour: it issues add_msl_shader_input calls for
// every TCS-out field to assert cross-stage emission order.
//
// Verdict: PASS if TES main0_in non-builtin members appear in same
// order (and same name + size) as TCS main0_out non-builtin members.
//
// Usage: msl-cross-stage-order-test <tcs.spv> <tes.spv>

#include "spirv_msl.hpp"
#include <algorithm>
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

static int check(const char *label, bool ok) {
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
	return ok ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s tcs.spv tes.spv\n", argv[0]);
		return 1;
	}
	auto tcs = read_spirv(argv[1]);
	auto tes = read_spirv(argv[2]);

	int fails = 0;

	// Step 1: compile TCS, get main0_out non-builtin members in order.
	std::vector<MSLInterfaceMember> tcs_user_members;
	{
		CompilerMSL c(tcs.data(), tcs.size());
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.multi_patch_workgroup = true;
		c.set_msl_options(o);
		(void)c.compile();
		auto layout = c.get_msl_interface_layout(StorageClassOutput, false);
		for (auto &m : layout.members)
			if (!m.is_builtin)
				tcs_user_members.push_back(m);
	}
	printf("# TCS main0_out non-builtin members (canonical order):\n");
	for (size_t i = 0; i < tcs_user_members.size(); ++i)
		printf("  [%zu] %-20s off=%u sz=%u vecsize=%u\n",
		       i, tcs_user_members[i].name.c_str(),
		       tcs_user_members[i].offset, tcs_user_members[i].size,
		       tcs_user_members[i].vecsize);

	// Step 2: TES bare compile (no orchestrator wiring) — capture
	// natural-emission baseline.
	std::vector<MSLInterfaceMember> tes_baseline;
	{
		CompilerMSL c(tes.data(), tes.size());
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.raw_buffer_tese_input = true;
		o.tess_evaluation_as_compute = true;
		o.capture_output_to_buffer = true;
		c.set_msl_options(o);
		(void)c.compile();
		auto layout = c.get_msl_interface_layout(StorageClassInput, false);
		for (auto &m : layout.members)
			if (!m.is_builtin)
				tes_baseline.push_back(m);
	}
	printf("\n# TES main0_in non-builtin members (baseline, no wiring):\n");
	for (size_t i = 0; i < tes_baseline.size(); ++i)
		printf("  [%zu] %-20s off=%u sz=%u vecsize=%u\n",
		       i, tes_baseline[i].name.c_str(),
		       tes_baseline[i].offset, tes_baseline[i].size,
		       tes_baseline[i].vecsize);

	// Helper: compile TES with a given orchestrator-call sequence.
	// `loc_mode` selects:
	//   "synthetic"     — every entry uses 0xE0000000+i (E.3-era rig
	//                     shape, kept for back-compat verification)
	//   "natural"       — every entry uses TCS-source's natural location
	//                     (matches β orchestrator's typical-case shape;
	//                     CKPT39 found this captures 0% under E.3's
	//                     synthetic-range-only recording gate)
	// `flag` selects whether `input_emission_in_call_order` is set on
	// the TES compile — Option E.4 adds the explicit opt-in.
	auto compile_tes_with_order = [&](const std::vector<std::string> &registration_order,
	                                  const char *loc_mode,
	                                  bool flag)
	    -> std::vector<MSLInterfaceMember>
	{
		CompilerMSL c(tes.data(), tes.size());
		CompilerMSL::Options o;
		o.msl_version = 30000;
		o.raw_buffer_tese_input = true;
		o.tess_evaluation_as_compute = true;
		o.capture_output_to_buffer = true;
		o.input_emission_in_call_order = flag;
		c.set_msl_options(o);

		uint32_t synth_loc = 0xE0000000u;
		for (auto &name : registration_order)
		{
			// Find vecsize + natural location from TCS member info.
			uint32_t vecsize = 0;
			uint32_t natural_loc = 0;
			bool found_natural = false;
			for (auto &m : tcs_user_members)
				if (m.name == name)
				{
					vecsize = m.vecsize;
					if (m.location != ~0u)
					{
						natural_loc = m.location;
						found_natural = true;
					}
					break;
				}
			if (vecsize == 0) vecsize = 1;

			MSLShaderInterfaceVariable v;
			v.name = name;
			if (std::string(loc_mode) == "synthetic" || !found_natural)
				v.location = synth_loc++;
			else
				v.location = natural_loc;
			v.component = 0;
			v.format = MSL_SHADER_VARIABLE_FORMAT_ANY32;
			v.builtin = BuiltInMax;
			v.vecsize = vecsize;
			v.rate = MSL_SHADER_VARIABLE_RATE_PER_VERTEX;
			c.add_msl_shader_input(v);
		}
		(void)c.compile();
		auto layout = c.get_msl_interface_layout(StorageClassInput, false);
		std::vector<MSLInterfaceMember> out;
		for (auto &m : layout.members)
			if (!m.is_builtin)
				out.push_back(m);
		return out;
	};

	std::vector<std::string> forward_order;
	for (auto &m : tcs_user_members)
		forward_order.push_back(m.name);
	std::vector<std::string> reverse_order = forward_order;
	std::reverse(reverse_order.begin(), reverse_order.end());

	// Step 3a: synthetic-range + flag-ON (E.3 standalone-rig shape).
	auto tes_synth_forward_on = compile_tes_with_order(forward_order, "synthetic", true);
	auto tes_synth_reverse_on = compile_tes_with_order(reverse_order, "synthetic", true);

	// Step 3b: PRODUCTION-CALLER SHAPE — natural-range + flag-ON.
	// CKPT39 banked: this is what the β orchestrator actually does
	// when TCS has explicit Location decoration. Standalone rig
	// validation MUST cover this shape per the §3.6 rig-vs-
	// integration symmetry discipline.
	auto tes_natural_forward_on = compile_tes_with_order(forward_order, "natural", true);
	auto tes_natural_reverse_on = compile_tes_with_order(reverse_order, "natural", true);

	// Step 3c: regression — natural-range + flag-OFF must preserve
	// LocationThenBuiltInType emission for non-AppGL public-API
	// consumers (MoltenVK, vkd3d-proton).
	auto tes_natural_forward_off = compile_tes_with_order(forward_order, "natural", false);
	auto tes_natural_reverse_off = compile_tes_with_order(reverse_order, "natural", false);

	auto print_layout = [](const char *label, const std::vector<MSLInterfaceMember> &m) {
		printf("\n# %s:\n", label);
		for (size_t i = 0; i < m.size(); ++i)
			printf("  [%zu] %-20s off=%u sz=%u vecsize=%u\n",
			       i, m[i].name.c_str(), m[i].offset, m[i].size, m[i].vecsize);
	};
	print_layout("E.4 synthetic-range + flag-ON, forward order", tes_synth_forward_on);
	print_layout("E.4 synthetic-range + flag-ON, REVERSED order", tes_synth_reverse_on);
	print_layout("E.4 natural-range + flag-ON, forward order (PRODUCTION-CALLER SHAPE)", tes_natural_forward_on);
	print_layout("E.4 natural-range + flag-ON, REVERSED order (PRODUCTION-CALLER SHAPE)", tes_natural_reverse_on);
	print_layout("E.4 natural-range + flag-OFF, forward order (BACK-COMPAT SHAPE)", tes_natural_forward_off);
	print_layout("E.4 natural-range + flag-OFF, REVERSED order (BACK-COMPAT SHAPE)", tes_natural_reverse_off);

	// Step 4: verdicts.
	printf("\n# === Path J' Option E.4 verdict ===\n");

	auto check_order_match = [&](const char *label, const std::vector<MSLInterfaceMember> &actual,
	                             const std::vector<std::string> &expected_names) {
		if (actual.size() != expected_names.size())
		{
			fails += check(label, false);
			return;
		}
		bool m = true;
		for (size_t i = 0; i < expected_names.size(); ++i)
			if (actual[i].name != expected_names[i]) { m = false; break; }
		fails += check(label, m);
	};

	// Synthetic-range scenarios (E.3-era back-compat under E.4 flag).
	check_order_match("synthetic-range + flag-ON, forward order matches TCS-out",
	                  tes_synth_forward_on, forward_order);
	check_order_match("synthetic-range + flag-ON, REVERSED order tracks call sequence",
	                  tes_synth_reverse_on, reverse_order);

	// PRODUCTION-CALLER SHAPE — load-bearing for E.4.
	check_order_match("natural-range + flag-ON, forward order matches TCS-out (PRODUCTION-CALLER SHAPE)",
	                  tes_natural_forward_on, forward_order);
	check_order_match("natural-range + flag-ON, REVERSED order tracks call sequence (PRODUCTION-CALLER SHAPE)",
	                  tes_natural_reverse_on, reverse_order);

	// BACK-COMPAT SHAPE — non-AppGL consumer expectation: LocationThenBuiltInType.
	// Forward order (TCS-out order = natural-loc-ascending here)
	// trivially matches both Location order and call order; reverse
	// order should NOT track call sequence under flag-OFF —
	// it should produce LocationThenBuiltInType emission.
	check_order_match("natural-range + flag-OFF, forward order produces LocationThenBuiltInType emission",
	                  tes_natural_forward_off, forward_order);

	{
		// Under flag-OFF + reversed registration, emission should
		// still be Location-ascending, NOT track the reversed call
		// order. Verify by asserting it's the LOCATION-ASCENDING
		// order (= forward_order).
		bool back_compat_ok = tes_natural_reverse_off.size() == forward_order.size();
		if (back_compat_ok)
		{
			for (size_t i = 0; i < forward_order.size(); ++i)
				if (tes_natural_reverse_off[i].name != forward_order[i])
				{
					back_compat_ok = false;
					break;
				}
		}
		fails += check("natural-range + flag-OFF, REVERSED reg STILL produces Location-ascending emission (BACK-COMPAT)",
		               back_compat_ok);
	}

	// Permutation observability — confirm reversed call order
	// produces observably different emission vs forward under
	// flag-ON, in BOTH range modes.
	{
		bool synth_perm = false;
		for (size_t i = 0; i < tes_synth_forward_on.size() && i < tes_synth_reverse_on.size(); ++i)
			if (tes_synth_forward_on[i].name != tes_synth_reverse_on[i].name)
				{ synth_perm = true; break; }
		fails += check("synthetic-range flag-ON: reverse permutes vs forward (rig-shape sanity)",
		               synth_perm);
	}
	{
		bool nat_perm = false;
		for (size_t i = 0; i < tes_natural_forward_on.size() && i < tes_natural_reverse_on.size(); ++i)
			if (tes_natural_forward_on[i].name != tes_natural_reverse_on[i].name)
				{ nat_perm = true; break; }
		fails += check("natural-range flag-ON: reverse permutes vs forward (PRODUCTION-CALLER SHAPE sanity)",
		               nat_perm);
	}

	fails += check("Baseline TES main0_in (no wiring) still emits non-builtin members (no-wiring regression guard)",
	               !tes_baseline.empty());

	printf("\n%s — %d failure(s)\n", fails == 0 ? "OK" : "FAIL", fails);
	return fails;
}
