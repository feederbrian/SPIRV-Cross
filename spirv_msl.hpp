/*
 * Copyright 2016-2021 The Brenwill Workshop Ltd.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * At your option, you may choose to accept this material under either:
 *  1. The Apache License, Version 2.0, found at <http://www.apache.org/licenses/LICENSE-2.0>, or
 *  2. The MIT License, found at <http://opensource.org/licenses/MIT>.
 */

#ifndef SPIRV_CROSS_MSL_HPP
#define SPIRV_CROSS_MSL_HPP

#include "spirv_glsl.hpp"
#include <map>
#include <set>
#include <stddef.h>
#include <unordered_map>
#include <unordered_set>

namespace SPIRV_CROSS_NAMESPACE
{
using namespace SPIRV_CROSS_SPV_HEADER_NAMESPACE;

// Indicates the format of a shader interface variable. Currently limited to specifying
// if the input is an 8-bit unsigned integer, 16-bit unsigned integer, or
// some other format.
enum MSLShaderVariableFormat
{
	MSL_SHADER_VARIABLE_FORMAT_OTHER = 0,
	MSL_SHADER_VARIABLE_FORMAT_UINT8 = 1,
	MSL_SHADER_VARIABLE_FORMAT_UINT16 = 2,
	MSL_SHADER_VARIABLE_FORMAT_ANY16 = 3,
	MSL_SHADER_VARIABLE_FORMAT_ANY32 = 4,

	// Deprecated aliases.
	MSL_VERTEX_FORMAT_OTHER = MSL_SHADER_VARIABLE_FORMAT_OTHER,
	MSL_VERTEX_FORMAT_UINT8 = MSL_SHADER_VARIABLE_FORMAT_UINT8,
	MSL_VERTEX_FORMAT_UINT16 = MSL_SHADER_VARIABLE_FORMAT_UINT16,
	MSL_SHADER_INPUT_FORMAT_OTHER = MSL_SHADER_VARIABLE_FORMAT_OTHER,
	MSL_SHADER_INPUT_FORMAT_UINT8 = MSL_SHADER_VARIABLE_FORMAT_UINT8,
	MSL_SHADER_INPUT_FORMAT_UINT16 = MSL_SHADER_VARIABLE_FORMAT_UINT16,
	MSL_SHADER_INPUT_FORMAT_ANY16 = MSL_SHADER_VARIABLE_FORMAT_ANY16,
	MSL_SHADER_INPUT_FORMAT_ANY32 = MSL_SHADER_VARIABLE_FORMAT_ANY32,

	MSL_SHADER_VARIABLE_FORMAT_INT_MAX = 0x7fffffff
};

// Indicates the rate at which a variable changes value, one of: per-vertex,
// per-primitive, or per-patch.
enum MSLShaderVariableRate
{
	MSL_SHADER_VARIABLE_RATE_PER_VERTEX = 0,
	MSL_SHADER_VARIABLE_RATE_PER_PRIMITIVE = 1,
	MSL_SHADER_VARIABLE_RATE_PER_PATCH = 2,

	MSL_SHADER_VARIABLE_RATE_INT_MAX = 0x7fffffff,
};

// Defines MSL characteristics of a shader interface variable at a particular location.
// After compilation, it is possible to query whether or not this location was used.
// If vecsize is nonzero, it must be greater than or equal to the vecsize declared in the shader,
// or behavior is undefined.
struct MSLShaderInterfaceVariable
{
	uint32_t location = 0;
	uint32_t component = 0;
	MSLShaderVariableFormat format = MSL_SHADER_VARIABLE_FORMAT_OTHER;
	BuiltIn builtin = BuiltInMax;
	uint32_t vecsize = 0;
	MSLShaderVariableRate rate = MSL_SHADER_VARIABLE_RATE_PER_VERTEX;
	// AppGL fork addition: optional name of the corresponding source-level
	// varying. Used by `split_tcs_outputs_by_consumption` to do name-match
	// classification across stages (separable programs auto-assign
	// locations per-stage independently, so location alone is not a stable
	// cross-stage identifier). Empty string = no name available; classifier
	// falls back to location-based matching where possible.
	std::string name;
};

// AppGL fork (msl-interface-introspection.patch): post-compile() readout of
// the canonical MSL stage-interface struct. Lets the caller (AppGL runtime)
// reflect the actual emitted layout instead of synthesizing a parallel one
// from SPIR-V — eliminates "AppGL mirror of SPIRV-Cross logic decays as
// patches evolve" bugs.
struct MSLInterfaceMember
{
	std::string name;        // member identifier as emitted in MSL
	uint32_t location = ~0u; // SPIR-V Location decoration; ~0u when builtin or implicit
	uint32_t component = 0;  // SPIR-V Component decoration; 0 unless multi-scalar packing at one location
	BuiltIn builtin = BuiltInMax; // BuiltInMax for user varyings; otherwise the gl_* builtin
	bool is_builtin = false;
	SPIRType::BaseType base_type = SPIRType::Unknown; // Float / Int / UInt / Half / etc.
	uint32_t bit_width = 0;  // 8, 16, 32, 64
	uint32_t vecsize = 0;    // 1=scalar, 2=vec2, 3=vec3, 4=vec4
	uint32_t columns = 0;    // matrix columns; 1 for non-matrix
	uint32_t array_size = 0; // 0 if not an array, else element count
	uint32_t offset = 0;     // bytes from struct base, per Metal packing
	uint32_t size = 0;       // bytes; for arrays, the total array size
};

// Layout result: members + the padded total size + struct alignment that
// callers need for stride math (TF buffer writes, gl_in[i] indexing, etc.).
// `struct_size` is rounded up to `struct_alignment` per Metal C ABI rules,
// matching the `sizeof(struct)` value the MSL compiler computes.
// `unpadded_end` is the offset+size of the last member without trailing
// padding; rarely useful but exposed for diagnostic purposes.
struct MSLInterfaceLayout
{
	std::vector<MSLInterfaceMember> members;
	uint32_t struct_size = 0;
	uint32_t struct_alignment = 0;
	uint32_t unpadded_end = 0;
};

// Matches the binding index of a MSL resource for a binding within a descriptor set.
// Taken together, the stage, desc_set and binding combine to form a reference to a resource
// descriptor used in a particular shading stage. The count field indicates the number of
// resources consumed by this binding, if the binding represents an array of resources.
// If the resource array is a run-time-sized array, which are legal in GLSL or SPIR-V, this value
// will be used to declare the array size in MSL, which does not support run-time-sized arrays.
// If pad_argument_buffer_resources is enabled, the base_type and count values are used to
// specify the base type and array size of the resource in the argument buffer, if that resource
// is not defined and used by the shader. With pad_argument_buffer_resources enabled, this
// information will be used to pad the argument buffer structure, in order to align that
// structure consistently for all uses, across all shaders, of the descriptor set represented
// by the arugment buffer. If pad_argument_buffer_resources is disabled, base_type does not
// need to be populated, and if the resource is also not a run-time sized array, the count
// field does not need to be populated.
// If using MSL 2.0 argument buffers, the descriptor set is not marked as a discrete descriptor set,
// and (for iOS only) the resource is not a storage image (sampled != 2), the binding reference we
// remap to will become an [[id(N)]] attribute within the "descriptor set" argument buffer structure.
// For resources which are bound in the "classic" MSL 1.0 way or discrete descriptors, the remap will
// become a [[buffer(N)]], [[texture(N)]] or [[sampler(N)]] depending on the resource types used.
struct MSLResourceBinding
{
	ExecutionModel stage = ExecutionModelMax;
	SPIRType::BaseType basetype = SPIRType::Unknown;
	uint32_t desc_set = 0;
	uint32_t binding = 0;
	uint32_t count = 0;
	uint32_t msl_buffer = 0;
	uint32_t msl_texture = 0;
	uint32_t msl_sampler = 0;
};

enum MSLSamplerCoord
{
	MSL_SAMPLER_COORD_NORMALIZED = 0,
	MSL_SAMPLER_COORD_PIXEL = 1,
	MSL_SAMPLER_INT_MAX = 0x7fffffff
};

enum MSLSamplerFilter
{
	MSL_SAMPLER_FILTER_NEAREST = 0,
	MSL_SAMPLER_FILTER_LINEAR = 1,
	MSL_SAMPLER_FILTER_INT_MAX = 0x7fffffff
};

enum MSLSamplerMipFilter
{
	MSL_SAMPLER_MIP_FILTER_NONE = 0,
	MSL_SAMPLER_MIP_FILTER_NEAREST = 1,
	MSL_SAMPLER_MIP_FILTER_LINEAR = 2,
	MSL_SAMPLER_MIP_FILTER_INT_MAX = 0x7fffffff
};

enum MSLSamplerAddress
{
	MSL_SAMPLER_ADDRESS_CLAMP_TO_ZERO = 0,
	MSL_SAMPLER_ADDRESS_CLAMP_TO_EDGE = 1,
	MSL_SAMPLER_ADDRESS_CLAMP_TO_BORDER = 2,
	MSL_SAMPLER_ADDRESS_REPEAT = 3,
	MSL_SAMPLER_ADDRESS_MIRRORED_REPEAT = 4,
	MSL_SAMPLER_ADDRESS_INT_MAX = 0x7fffffff
};

enum MSLSamplerCompareFunc
{
	MSL_SAMPLER_COMPARE_FUNC_NEVER = 0,
	MSL_SAMPLER_COMPARE_FUNC_LESS = 1,
	MSL_SAMPLER_COMPARE_FUNC_LESS_EQUAL = 2,
	MSL_SAMPLER_COMPARE_FUNC_GREATER = 3,
	MSL_SAMPLER_COMPARE_FUNC_GREATER_EQUAL = 4,
	MSL_SAMPLER_COMPARE_FUNC_EQUAL = 5,
	MSL_SAMPLER_COMPARE_FUNC_NOT_EQUAL = 6,
	MSL_SAMPLER_COMPARE_FUNC_ALWAYS = 7,
	MSL_SAMPLER_COMPARE_FUNC_INT_MAX = 0x7fffffff
};

enum MSLSamplerBorderColor
{
	MSL_SAMPLER_BORDER_COLOR_TRANSPARENT_BLACK = 0,
	MSL_SAMPLER_BORDER_COLOR_OPAQUE_BLACK = 1,
	MSL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE = 2,
	MSL_SAMPLER_BORDER_COLOR_INT_MAX = 0x7fffffff
};

enum MSLFormatResolution
{
	MSL_FORMAT_RESOLUTION_444 = 0,
	MSL_FORMAT_RESOLUTION_422,
	MSL_FORMAT_RESOLUTION_420,
	MSL_FORMAT_RESOLUTION_INT_MAX = 0x7fffffff
};

enum MSLChromaLocation
{
	MSL_CHROMA_LOCATION_COSITED_EVEN = 0,
	MSL_CHROMA_LOCATION_MIDPOINT,
	MSL_CHROMA_LOCATION_INT_MAX = 0x7fffffff
};

enum MSLComponentSwizzle
{
	MSL_COMPONENT_SWIZZLE_IDENTITY = 0,
	MSL_COMPONENT_SWIZZLE_ZERO,
	MSL_COMPONENT_SWIZZLE_ONE,
	MSL_COMPONENT_SWIZZLE_R,
	MSL_COMPONENT_SWIZZLE_G,
	MSL_COMPONENT_SWIZZLE_B,
	MSL_COMPONENT_SWIZZLE_A,
	MSL_COMPONENT_SWIZZLE_INT_MAX = 0x7fffffff
};

enum MSLSamplerYCbCrModelConversion
{
	MSL_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY = 0,
	MSL_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_IDENTITY,
	MSL_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_BT_709,
	MSL_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_BT_601,
	MSL_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_BT_2020,
	MSL_SAMPLER_YCBCR_MODEL_CONVERSION_INT_MAX = 0x7fffffff
};

enum MSLSamplerYCbCrRange
{
	MSL_SAMPLER_YCBCR_RANGE_ITU_FULL = 0,
	MSL_SAMPLER_YCBCR_RANGE_ITU_NARROW,
	MSL_SAMPLER_YCBCR_RANGE_INT_MAX = 0x7fffffff
};

struct MSLConstexprSampler
{
	MSLSamplerCoord coord = MSL_SAMPLER_COORD_NORMALIZED;
	MSLSamplerFilter min_filter = MSL_SAMPLER_FILTER_NEAREST;
	MSLSamplerFilter mag_filter = MSL_SAMPLER_FILTER_NEAREST;
	MSLSamplerMipFilter mip_filter = MSL_SAMPLER_MIP_FILTER_NONE;
	MSLSamplerAddress s_address = MSL_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
	MSLSamplerAddress t_address = MSL_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
	MSLSamplerAddress r_address = MSL_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
	MSLSamplerCompareFunc compare_func = MSL_SAMPLER_COMPARE_FUNC_NEVER;
	MSLSamplerBorderColor border_color = MSL_SAMPLER_BORDER_COLOR_TRANSPARENT_BLACK;
	float lod_clamp_min = 0.0f;
	float lod_clamp_max = 1000.0f;
	int max_anisotropy = 1;

	// Sampler Y'CbCr conversion parameters
	uint32_t planes = 0;
	MSLFormatResolution resolution = MSL_FORMAT_RESOLUTION_444;
	MSLSamplerFilter chroma_filter = MSL_SAMPLER_FILTER_NEAREST;
	MSLChromaLocation x_chroma_offset = MSL_CHROMA_LOCATION_COSITED_EVEN;
	MSLChromaLocation y_chroma_offset = MSL_CHROMA_LOCATION_COSITED_EVEN;
	MSLComponentSwizzle swizzle[4]; // IDENTITY, IDENTITY, IDENTITY, IDENTITY
	MSLSamplerYCbCrModelConversion ycbcr_model = MSL_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
	MSLSamplerYCbCrRange ycbcr_range = MSL_SAMPLER_YCBCR_RANGE_ITU_FULL;
	uint32_t bpc = 8;

	bool compare_enable = false;
	bool lod_clamp_enable = false;
	bool anisotropy_enable = false;
	bool ycbcr_conversion_enable = false;

	MSLConstexprSampler()
	{
		for (uint32_t i = 0; i < 4; i++)
			swizzle[i] = MSL_COMPONENT_SWIZZLE_IDENTITY;
	}
	bool swizzle_is_identity() const
	{
		return (swizzle[0] == MSL_COMPONENT_SWIZZLE_IDENTITY && swizzle[1] == MSL_COMPONENT_SWIZZLE_IDENTITY &&
		        swizzle[2] == MSL_COMPONENT_SWIZZLE_IDENTITY && swizzle[3] == MSL_COMPONENT_SWIZZLE_IDENTITY);
	}
	bool swizzle_has_one_or_zero() const
	{
		return (swizzle[0] == MSL_COMPONENT_SWIZZLE_ZERO || swizzle[0] == MSL_COMPONENT_SWIZZLE_ONE ||
		        swizzle[1] == MSL_COMPONENT_SWIZZLE_ZERO || swizzle[1] == MSL_COMPONENT_SWIZZLE_ONE ||
		        swizzle[2] == MSL_COMPONENT_SWIZZLE_ZERO || swizzle[2] == MSL_COMPONENT_SWIZZLE_ONE ||
		        swizzle[3] == MSL_COMPONENT_SWIZZLE_ZERO || swizzle[3] == MSL_COMPONENT_SWIZZLE_ONE);
	}
};

// Special constant used in a MSLResourceBinding desc_set
// element to indicate the bindings for the push constants.
// Kinda deprecated. Just use ResourceBindingPushConstant{DescriptorSet,Binding} directly.
static const uint32_t kPushConstDescSet = ResourceBindingPushConstantDescriptorSet;

// Special constant used in a MSLResourceBinding binding
// element to indicate the bindings for the push constants.
// Kinda deprecated. Just use ResourceBindingPushConstant{DescriptorSet,Binding} directly.
static const uint32_t kPushConstBinding = ResourceBindingPushConstantBinding;

// Special constant used in a MSLResourceBinding binding
// element to indicate the buffer binding for swizzle buffers.
static const uint32_t kSwizzleBufferBinding = ~(1u);

// Special constant used in a MSLResourceBinding binding
// element to indicate the buffer binding for buffer size buffers to support OpArrayLength.
static const uint32_t kBufferSizeBufferBinding = ~(2u);

// Special constant used in a MSLResourceBinding binding
// element to indicate the buffer binding used for the argument buffer itself.
// This buffer binding should be kept as small as possible as all automatic bindings for buffers
// will start at max(kArgumentBufferBinding) + 1.
static const uint32_t kArgumentBufferBinding = ~(3u);

static const uint32_t kMaxArgumentBuffers = 8;

// Decompiles SPIR-V to Metal Shading Language
class CompilerMSL : public CompilerGLSL
{
public:
	// Options for compiling to Metal Shading Language
	struct Options
	{
		typedef enum
		{
			iOS = 0,
			macOS = 1
		} Platform;

		Platform platform = macOS;
		uint32_t msl_version = make_msl_version(1, 2);
		uint32_t texel_buffer_texture_width = 4096; // Width of 2D Metal textures used as 1D texel buffers
		uint32_t r32ui_linear_texture_alignment = 4;
		uint32_t r32ui_alignment_constant_id = 65535;
		uint32_t swizzle_buffer_index = 30;
		uint32_t indirect_params_buffer_index = 29;
		uint32_t shader_output_buffer_index = 28;
		uint32_t shader_patch_output_buffer_index = 27;
		uint32_t shader_tess_factor_buffer_index = 26;
		uint32_t buffer_size_buffer_index = 25;
		uint32_t view_mask_buffer_index = 24;
		uint32_t dynamic_offsets_buffer_index = 23;
		uint32_t shader_input_buffer_index = 22;
		uint32_t shader_index_buffer_index = 21;
		uint32_t shader_patch_input_buffer_index = 20;
		uint32_t shader_input_wg_index = 0;
		uint32_t device_index = 0;
		uint32_t enable_frag_output_mask = 0xffffffff;
		// Metal doesn't allow setting a fixed sample mask directly in the pipeline.
		// We can evade this restriction by ANDing the internal sample_mask output
		// of the shader with the additional fixed sample mask.
		uint32_t additional_fixed_sample_mask = 0xffffffff;
		bool enable_point_size_builtin = true;
		bool enable_point_size_default = false;
		float default_point_size = 1.0f;
		bool enable_frag_depth_builtin = true;
		bool enable_frag_stencil_ref_builtin = true;
		bool disable_rasterization = false;
		bool capture_output_to_buffer = false;
		bool swizzle_texture_samples = false;
		bool tess_domain_origin_lower_left = false;
		bool multiview = false;
		bool multiview_layered_rendering = true;
		bool view_index_from_device_index = false;
		bool dispatch_base = false;
		bool texture_1D_as_2D = false;

		// Enable use of Metal argument buffers.
		// MSL 2.0 must also be enabled.
		bool argument_buffers = false;

		// Defines Metal argument buffer tier levels.
		// Uses same values as Metal MTLArgumentBuffersTier enumeration.
		enum class ArgumentBuffersTier
		{
			Tier1 = 0,
			Tier2 = 1,
		};

		// When using Metal argument buffers, indicates the Metal argument buffer tier level supported by the Metal platform.
		// Ignored when Options::argument_buffers is disabled.
		// - Tier1 supports writable images on macOS, but not on iOS.
		// - Tier2 supports writable images on macOS and iOS, and higher resource count limits.
		// Tier capabilities based on recommendations from Apple engineering.
		ArgumentBuffersTier argument_buffers_tier = ArgumentBuffersTier::Tier1;

		// Enables specifick argument buffer format with extra information to track SSBO-length
		bool runtime_array_rich_descriptor = false;

		// Ensures vertex and instance indices start at zero. This reflects the behavior of HLSL with SV_VertexID and SV_InstanceID.
		bool enable_base_index_zero = false;

		// Fragment output in MSL must have at least as many components as the render pass.
		// Add support to explicit pad out components.
		bool pad_fragment_output_components = false;

		// Specifies whether the iOS target version supports the [[base_vertex]] and [[base_instance]] attributes.
		bool ios_support_base_vertex_instance = false;

		// Use Metal's native frame-buffer fetch API for subpass inputs.
		bool use_framebuffer_fetch_subpasses = false;

		// Enables use of "fma" intrinsic for invariant float math
		bool invariant_float_math = false;

		// Emulate texturecube_array with texture2d_array for iOS where this type is not available
		bool emulate_cube_array = false;

		// Allow user to enable decoration binding
		bool enable_decoration_binding = false;

		// Requires MSL 2.1, use the native support for texel buffers.
		bool texture_buffer_native = false;

		// Forces all resources which are part of an argument buffer to be considered active.
		// This ensures ABI compatibility between shaders where some resources might be unused,
		// and would otherwise declare a different IAB.
		bool force_active_argument_buffer_resources = false;

		// Aligns each resource in an argument buffer to its assigned index value, id(N),
		// by adding synthetic padding members in the argument buffer struct for any resources
		// in the argument buffer that are not defined and used by the shader. This allows
		// the shader to index into the correct argument in a descriptor set argument buffer
		// that is shared across shaders, where not all resources in the argument buffer are
		// defined in each shader. For this to work, an MSLResourceBinding must be provided for
		// all descriptors in any descriptor set held in an argument buffer in the shader, and
		// that MSLResourceBinding must have the basetype and count members populated correctly.
		// The implementation here assumes any inline blocks in the argument buffer is provided
		// in a Metal buffer, and doesn't take into consideration inline blocks that are
		// optionally embedded directly into the argument buffer via add_inline_uniform_block().
		bool pad_argument_buffer_resources = false;

		// Forces the use of plain arrays, which works around certain driver bugs on certain versions
		// of Intel Macbooks. See https://github.com/KhronosGroup/SPIRV-Cross/issues/1210.
		// May reduce performance in scenarios where arrays are copied around as value-types.
		bool force_native_arrays = false;

		// If a shader writes clip distance, also emit user varyings which
		// can be read in subsequent stages.
		bool enable_clip_distance_user_varying = true;

		// In a tessellation control shader, assume that more than one patch can be processed in a
		// single workgroup. This requires changes to the way the InvocationId and PrimitiveId
		// builtins are processed, but should result in more efficient usage of the GPU.
		bool multi_patch_workgroup = false;

		// Use storage buffers instead of vertex-style attributes for tessellation evaluation
		// input. This may require conversion of inputs in the generated post-tessellation
		// vertex shader, but allows the use of nested arrays.
		bool raw_buffer_tese_input = false;

		// If set, a vertex shader will be compiled as part of a tessellation pipeline.
		// It will be translated as a compute kernel, so it can use the global invocation ID
		// to index the output buffer.
		bool vertex_for_tessellation = false;

		// AppGL fork: lower SPIR-V FP64 values to a legal Metal df64 transport
		// representation. Metal does not expose native fp64 arithmetic on Apple
		// GPUs, so this option maps double/dvec/dmat types to AppGL helper
		// structs backed by uint2 words and emits helper calls for arithmetic.
		// Runtime extension advertising remains owned by AppGL's FP64 module.
		bool appgl_fp64_emulation = false;

		// AppGL fork addition (phase-3B-metal-tess-TF): when set, a
		// tessellation evaluation shader is translated as a compute
		// kernel rather than a `[[patch(...)]] vertex` function.
		// Used by the AppGL transform-feedback capture path, which
		// dispatches one TES kernel thread per tessellator-generated
		// vertex (the caller supplies the domain coords in a buffer;
		// the kernel writes per-vertex TF output into a user-bound
		// buffer). Companion to `vertex_for_tessellation` but for
		// the post-tessellator stage.
		//
		// Requires `raw_buffer_tese_input = true` so gl_in / per-patch
		// inputs already live in buffers (buffer slots 22 / 20).
		// Output is written through a new TF-capture struct at
		// `shader_output_buffer_index` (buffer 28 by default).
		//
		// Orthogonal to isoline-tessellation support, which Metal
		// does not implement natively — the compute form still
		// throws on isolines. Domain-point generation for isolines
		// happens on the AppGL side.
		bool tess_evaluation_as_compute = false;

		// AppGL fork: per-patch control-point count for TES compilation.
		// The TES's own IR has no output_vertices; this value is the
		// TCS's `layout(vertices = N) out;` and is needed to emit the
		// correct stride in
		//   gl_in = &spvIn[gl_PrimitiveID * N];
		// (was `* 0` when unset, collapsing all patches to the origin).
		// 0 = fall back to existing behaviour.
		uint32_t tese_input_patch_vertices = 0;

		// AppGL fork addition (msl-tcs-output-classification.patch):
		// classify each TCS user-varying output by whether the linked
		// downstream TES consumes it. TES-consumed outputs route to the
		// per-CP device buffer (the existing default behaviour).
		// TCS-internal outputs — those used purely for inter-invocation
		// sync via `barrier()` and never read by TES — are masked out
		// of the per-CP buffer struct and routed to **threadgroup
		// memory** instead. Threadgroup memory is the natural Metal
		// home for inter-invocation sync data: it doesn't leak into the
		// device buffer the TES reads, doesn't bloat the per-CP stride,
		// and exists for the lifetime of the workgroup which matches a
		// TCS patch.
		//
		// Classification mechanism: a TCS output is TES-consumed iff
		// its location appears in the `outputs_by_location` map (or its
		// builtin appears in `outputs_by_builtin`), which the AppGL
		// runtime populates via `add_msl_shader_output` calls based on
		// the linked TES's input interface. Outputs absent from those
		// maps are TCS-internal.
		//
		// Implementation: leverages SPIRV-Cross's existing
		// `mask_stage_output_by_location` / `_by_builtin` API to flag
		// TCS-internal outputs as "masked"; the existing
		// `variable_decl_is_remapped_storage(StorageClassWorkgroup)`
		// path then routes masked TCS outputs to threadgroup memory at
		// emission time. No new emission code paths required.
		//
		// Use case: separable-program tess pipelines where the TCS uses
		// `out` arrays + `barrier()` for inter-invocation
		// coordination (e.g. CTS `tessellation_shader_tessellation.{
		// barrier_guarded_read_calls, barrier_guarded_read_write_calls,
		// input_patch_discard }` cluster). Without this option, the
		// per-CP buffer stride balloons with TCS-internal arrays the
		// TES never reads, breaking byte-offset alignment between
		// TCS-out and TES-in.
		//
		// Default false (existing behaviour: all TCS outputs in the
		// per-CP device buffer).
		bool split_tcs_outputs_by_consumption = false;

		// AppGL fork addition (msl-geometry-shader-as-mesh.patch): emit
		// geometry shaders (`ExecutionModelGeometry`) as Metal mesh
		// shaders (`[[mesh]] void main0(...)`) instead of the current
		// non-functional vertex-form emission. Targets Metal devices
		// with mesh shader support (Apple GPU family 7+ / Metal 3+);
		// chips lacking support fall back to the AppGL CPU geometry
		// shader interpreter via runtime capability detection.
		//
		// Emission strategy: each GS invocation maps to one mesh
		// threadgroup with a single thread. `OpEmitVertex` becomes a
		// member-by-member copy of the per-vertex output struct into
		// `gl_MeshVerticesEXT[spvVertexIndex]` followed by `++spvVertexIndex`.
		// `OpEndPrimitive` writes triangle/line/point indices into
		// `gl_PrimitiveTriangleIndicesEXT` (or LineIndicesEXT /
		// PointIndicesEXT) based on the GS output topology, captures
		// per-primitive outputs (gl_Layer / gl_PrimitiveID / gl_ViewportIndex)
		// from the latched per-primitive struct, and increments
		// `spvPrimitiveIndex`. At function exit, `SetMeshOutputsEXT(
		// spvVertexIndex, spvPrimitiveIndex)` emits the meshlet sizes.
		//
		// Out of scope (deferred per Sprint 3 narrowed plan):
		//  - Streams (gl_StreamID 0-3): mesh shaders have no native
		//    multi-stream output; would need multi-dispatch or stream-
		//    tagged single-dispatch emulation.
		//  - Adjacency input topologies (`triangle_adjacency`,
		//    `line_adjacency`): no mesh shader equivalent; tests using
		//    these stay on the AppGL CPU GS interpreter.
		//  - GS-after-TES interaction (geometry shader running on TES
		//    output): cross-pipeline coordination outside this option's
		//    scope.
		//
		// Requires `msl_version >= 30000` (Metal 3.0); mesh shaders are
		// not available in earlier Metal versions.
		bool geometry_shader_as_mesh = false;

		// AppGL fork addition: emit `threadgroup_barrier(mem_flags::mem_device)`
		// at the end of a `vertex_for_tessellation`-mode VS-as-compute kernel.
		// Apple's AIR optimizer + Metal driver appear to silently eliminate
		// `device main0_out* spvOut` writes when the consumer is in a different
		// encoder family (compute → render mesh-pipeline) — writes survive
		// the compute → compute path (Phase-3 metal-tess), vanish on
		// compute → render (mesh-GS Phase 2). The end-of-kernel device-memory
		// barrier signals that all device writes are ordered before kernel
		// exit, blocking the AIR-layer elimination.
		//
		// Default off — preserves existing tess→tess byte-identical
		// emission. AppGL-W enables for VS-compute targeting mesh-pipeline
		// consumption; tess→tess remains unchanged. Uses
		// `mem_flags::mem_device` (not `mem_device_and_threadgroup`)
		// because no threadgroup memory is shared in this VS-compute path.
		//
		// **Empirical calibration (Path E):** the barrier alone was tested
		// insufficient against Apple's AIR cross-encoder-family elimination —
		// when no in-kernel use of spvOut is observable, the AIR optimizer
		// can eliminate writes + the barrier as a unit. The barrier flag is
		// preserved for calibration value (and for the case where future
		// Apple driver behavior changes); the load-bearing mitigation is
		// `force_compute_kernel_device_volatile_writes` below.
		bool force_compute_kernel_device_barrier_at_exit = false;

		// AppGL fork addition (Path E++): emit
		// `volatile device main0_out* spvOut [[buffer(28)]]` instead of
		// `device main0_out*` for `vertex_for_tessellation +
		// capture_output_to_buffer` VS-compute kernels. Per Apple's MSL spec
		// `volatile` is contractually preserved across optimizer passes —
		// each write through a `volatile`-qualified pointer must be observable
		// at its source location. This blocks the AIR cross-encoder-family
		// elimination class that the bare barrier (above) was empirically
		// insufficient to defeat.
		//
		// Default off — preserves byte-identical emission for the Phase-3
		// metal-tess (compute → compute) path. AppGL-W's runtime enables
		// (typically alongside `force_compute_kernel_device_barrier_at_exit`)
		// for VS-compute targeting mesh-pipeline consumption.
		bool force_compute_kernel_device_volatile_writes = false;

		// AppGL fork addition (Path E+++): convert each VS-compute write
		// to spvOut into `atomic_store_explicit` with a per-scalar
		// reinterpret to uint. Per Apple's MSL spec, atomic stores
		// contractually cannot be eliminated by the AIR optimizer —
		// the load-bearing strength tier escalation after barrier
		// (Path E) and volatile (Path E++) were both empirically
		// insufficient against AIR cross-encoder-family elimination.
		// `memory_order_relaxed` keeps overhead low (no cross-thread
		// ordering implied). Vec/array writes are decomposed into
		// per-scalar atomic stores. Default off — tess→tess
		// unchanged.
		bool force_compute_kernel_atomic_writes_on_spvOut = false;

		// AppGL fork addition (Path E+++ diagnostic): add a
		// `device atomic_uint* spvKernelEntryCounter [[buffer(N)]]`
		// parameter to VS-compute kernels and emit
		// `atomic_fetch_add_explicit(spvKernelEntryCounter, 1u,
		// memory_order_relaxed);` in the kernel preamble. Definitively
		// answers "did the kernel body execute" — if AppGL-W's
		// readback shows counter > 0 the kernel ran and any subsequent
		// buffer-state divergence is AIR/optimizer behavior; if
		// counter == 0 the dispatch was issued but the kernel body
		// didn't run, indicating a different-layer issue (binding,
		// resource tracking, etc.). Slot defaults to
		// `entry_counter_buffer_index` (27 by default) — orthogonal to
		// shader_output_buffer_index (28). Default off; orthogonal to
		// the atomic_writes flag — the probe can be used standalone.
		bool force_compute_kernel_entry_counter_probe = false;
		uint32_t entry_counter_buffer_index = 27;

		// AppGL fork addition (Path G — kernel-arg attribute fix):
		// emit `[[threads_per_grid]]` instead of `[[grid_size]]` for the
		// implicit `spvStageInputSize` parameter on `vertex_for_tessellation`
		// VS-compute kernels.
		//
		// `[[grid_size]]` is documented as the threadgroup grid dimensions
		// (the number-of-threadgroups argument to dispatchThreadgroups:).
		// On Apple Silicon under both `dispatchThreads:` and
		// `dispatchThreadgroups:` it returns `(0, 0, 0)` for these
		// kernel signatures — observed via kernel-internal probing
		// (write each builtin attribute to a buffer from thread 0 +
		// inspect from outside). Effect: every thread early-returns at
		// `if (any(gl_GlobalInvocationID >= spvStageInputSize)) return;`
		// because `any(>= 0)` is universally true. Kernel body never
		// runs; cmdBuf reports Completed; all external diagnostics
		// (capture inspector, PSO API queries, validation layers) show
		// clean. Path E barrier / E++ volatile / E+++ atomic
		// mitigations were all attempting to preserve writes from a
		// kernel body that never executed.
		//
		// `[[threads_per_grid]]` is the actual thread count of the
		// entire grid — what bounds checks should reference for
		// `dispatchThreads:`-shaped dispatches. Changes the synthesized
		// argument's attribute only; `gl_GlobalInvocationID` and
		// downstream code keep their existing forms.
		//
		// Default off — preserves byte-identical emission for the
		// Phase-3 metal-tess (compute → compute) path. AppGL-W's
		// runtime enables for both VS-compute paths (mesh-GS AND tess)
		// since the fix is more correct per Apple's MSL spec
		// semantics.
		bool force_threads_per_grid_for_stage_input_size = false;

		// AppGL fork addition (Path L — Sprint 4 sub-cluster A): emit
		// TES-as-compute reads of `gl_TessLevelOuter` / `gl_TessLevelInner`
		// from a full-precision `device const float*` shadow buffer
		// instead of from Metal's half-precision
		// `MTLQuadTessellationFactorsHalf` / `MTLTriangleTessellationFactorsHalf`
		// API (member access via `.edgeTessellationFactor[k]` /
		// `.insideTessellationFactor[k]`).
		//
		// Per GL 4.6 §11.2.2, outer/inner tess level values are float;
		// CTS validates with float precision. Metal's half-precision
		// API truncates information at write time (CKPT25 Option B
		// elimination demonstrated lossy-write). Full-precision shadow
		// buffer preserves the float precision the CTS expects.
		//
		// Buffer layout (per primitive, stride depends on domain):
		//   triangles: 4 floats = 3 outer + 1 inner
		//   quads:     6 floats = 4 outer + 2 inner
		//   isolines:  2 floats = 2 outer + 0 inner
		// Outer levels first, then inner levels per primitive.
		// Read pattern: `spvTessLevelFull[gl_PrimitiveID * stride + index]`.
		//
		// Flag-gated (not default-on) because consumers must provide the
		// shadow buffer infrastructure at the documented slot. AppGL fork
		// consumers + cross-project consumers (MoltenVK on Vulkan-on-Metal
		// has the same Metal half-precision constraint) can opt in;
		// default-off preserves byte-identical emission for existing
		// consumers without shadow buffer infrastructure.
		bool use_full_precision_tess_level_buffer = false;
		uint32_t shader_tess_factor_buffer_full_index = 23;

		// Path J' Option E.4 (Sprint 6 Phase 1 — paired Class 2A + 2C
		// extension after CKPT39 rig-vs-integration symmetry violation):
		// when set, `add_interface_block(StorageClassInput)` orders
		// `main0_in` members by the orchestrator's `add_msl_shader_input`
		// call sequence rather than by Location ascending. The flag is
		// the explicit opt-in gate for the
		// `MemberSorter::InsertionOrderThenLocationThenBuiltInType`
		// sort aspect introduced in Option E.3 (gate 20).
		//
		// Why a flag (vs unconditional widening): the public C API
		// `spvc_compiler_msl_add_shader_input` is used by upstream
		// consumers (MoltenVK, vkd3d-proton) who register their inputs
		// via this path. Unconditional widening would change emission
		// order for any such consumer. Default-off preserves
		// `LocationThenBuiltInType` emission for existing consumers;
		// AppGL fork orchestrators set this flag explicitly to opt
		// into call-order emission.
		//
		// Why explicit flag (vs synthetic-range gate from E.3): CKPT39
		// surfaced that AppGL-W's β orchestrator passes natural-range
		// locations when the TCS sibling has explicit Location decoration
		// (the typical case — `layout(location=N)` qualifiers, glslc-
		// assigned locations in monolithic programs), and only escalates
		// to synthetic-range (0xE0000000+id) when natural location is
		// absent. E.3's synthetic-range-gated recording therefore captured
		// 0% of typical production calls, leaving the insertion-order
		// list empty and the new aspect dormant — standalone rig PASS,
		// integration FAIL. The flag makes the gate explicit + caller-
		// agnostic. Recording (Class 2A) widens to all non-empty-name
		// calls regardless of range; emission (Class 2C) gates on the
		// flag.
		bool input_emission_in_call_order = false;

		// Assume that SubpassData images have multiple layers. Layered input attachments
		// are addressed relative to the Layer output from the vertex pipeline. This option
		// has no effect with multiview, since all input attachments are assumed to be layered
		// and will be addressed using the current ViewIndex.
		bool arrayed_subpass_input = false;

		// Whether to use SIMD-group or quadgroup functions to implement group non-uniform
		// operations. Some GPUs on iOS do not support the SIMD-group functions, only the
		// quadgroup functions.
		bool ios_use_simdgroup_functions = false;

		// If set, the subgroup size will be assumed to be one, and subgroup-related
		// builtins and operations will be emitted accordingly. This mode is intended to
		// be used by MoltenVK on hardware/software configurations which do not provide
		// sufficient support for subgroups.
		bool emulate_subgroups = false;

		// If nonzero, a fixed subgroup size to assume. Metal, similarly to VK_EXT_subgroup_size_control,
		// allows the SIMD-group size (aka thread execution width) to vary depending on
		// register usage and requirements. In certain circumstances--for example, a pipeline
		// in MoltenVK without VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT--
		// this is undesirable. This fixes the value of the SubgroupSize builtin, instead of
		// mapping it to the Metal builtin [[thread_execution_width]]. If the thread
		// execution width is reduced, the extra invocations will appear to be inactive.
		// If zero, the SubgroupSize will be allowed to vary, and the builtin will be mapped
		// to the Metal [[thread_execution_width]] builtin.
		uint32_t fixed_subgroup_size = 0;

		enum class IndexType
		{
			None = 0,
			UInt16 = 1,
			UInt32 = 2
		};

		// The type of index in the index buffer, if present. For a compute shader, Metal
		// requires specifying the indexing at pipeline creation, rather than at draw time
		// as with graphics pipelines. This means we must create three different pipelines,
		// for no indexing, 16-bit indices, and 32-bit indices. Each requires different
		// handling for the gl_VertexIndex builtin. We may as well, then, create three
		// different shaders for these three scenarios.
		IndexType vertex_index_type = IndexType::None;

		// If set, a dummy [[sample_id]] input is added to a fragment shader if none is present.
		// This will force the shader to run at sample rate, assuming Metal does not optimize
		// the extra threads away.
		bool force_sample_rate_shading = false;

		// If set, gl_HelperInvocation will be set manually whenever a fragment is discarded.
		// Some Metal devices have a bug where simd_is_helper_thread() does not return true
		// after a fragment has been discarded. This is a workaround that is only expected to be needed
		// until the bug is fixed in Metal; it is provided as an option to allow disabling it when that occurs.
		bool manual_helper_invocation_updates = true;

		// If set, extra checks will be emitted in fragment shaders to prevent writes
		// from discarded fragments. Some Metal devices have a bug where writes to storage resources
		// from discarded fragment threads continue to occur, despite the fragment being
		// discarded. This is a workaround that is only expected to be needed until the
		// bug is fixed in Metal; it is provided as an option so it can be enabled
		// only when the bug is present.
		bool check_discarded_frag_stores = false;

		// If set, Lod operands to OpImageSample*DrefExplicitLod for 1D and 2D array images
		// will be implemented using a gradient instead of passing the level operand directly.
		// Some Metal devices have a bug where the level() argument to depth2d_array<T>::sample_compare()
		// in a fragment shader is biased by some unknown amount, possibly dependent on the
		// partial derivatives of the texture coordinates. This is a workaround that is only
		// expected to be needed until the bug is fixed in Metal; it is provided as an option
		// so it can be enabled only when the bug is present.
		bool sample_dref_lod_array_as_grad = false;

		// MSL doesn't guarantee coherence between writes and subsequent reads of read_write textures.
		// This inserts fences before each read of a read_write texture to ensure coherency.
		// If you're sure you never rely on this, you can set this to false for a possible performance improvement.
		// Note: Only Apple's GPU compiler takes advantage of the lack of coherency, so make sure to test on Apple GPUs if you disable this.
		bool readwrite_texture_fences = true;

		// Metal 3.1 introduced a Metal regression bug which causes infinite recursion during 
		// Metal's analysis of an entry point input structure that is itself recursive. Enabling
		// this option will replace the recursive input declaration with a alternate variable of
		// type void*, and then cast to the correct type at the top of the entry point function.
		// The bug has been reported to Apple, and will hopefully be fixed in future releases.
		bool replace_recursive_inputs = false;

		// If set, manual fixups of gradient vectors for cube texture lookups will be performed.
		// All released Apple Silicon GPUs to date behave incorrectly when sampling a cube texture
		// with explicit gradients. They will ignore one of the three partial derivatives based
		// on the selected major axis, and expect the remaining derivatives to be partially
		// transformed.
		bool agx_manual_cube_grad_fixup = false;

		// Metal will discard fragments with side effects under certain circumstances prematurely.
		// Example: CTS test dEQP-VK.fragment_operations.early_fragment.discard_no_early_fragment_tests_depth
		// Test will render a full screen quad with varying depth [0,1] for each fragment.
		// Each fragment will do an operation with side effects, modify the depth value and
		// discard the fragment. The test expects the fragment to be run due to:
		// https://registry.khronos.org/vulkan/specs/1.0-extensions/html/vkspec.html#fragops-shader-depthreplacement
		// which states that the fragment shader must be run due to replacing the depth in shader.
		// However, Metal may prematurely discards fragments without executing them
		// (I believe this to be due to a greedy optimization on their end) making the test fail.
		// This option enforces fragment execution for such cases where the fragment has operations
		// with side effects. Provided as an option hoping Metal will fix this issue in the future.
		bool force_fragment_with_side_effects_execution = false;

		// If set, adds a depth pass through statement to circumvent the following issue:
		// When the same depth/stencil is used as input and depth/stencil attachment, we need to
		// force Metal to perform the depth/stencil write after fragment execution. Otherwise,
		// Metal will write to the depth attachment before fragment execution. This happens
		// if the fragment does not modify the depth value.
		bool input_attachment_is_ds_attachment = false;

		// If BuiltInPosition is not written, automatically disable rasterization.
		// The result can be queried with get_is_rasterization_disabled.
		bool auto_disable_rasterization = false;

		// Use Fast Math pragmas in MSL code, based on SPIR-V float controls and FP ExecutionModes.
		// Requires MSL 3.2 or above, and has no effect with earlier MSL versions.
		bool use_fast_math_pragmas = false;

		bool is_ios() const
		{
			return platform == iOS;
		}

		bool is_macos() const
		{
			return platform == macOS;
		}

		bool use_quadgroup_operation() const
		{
			return is_ios() && !ios_use_simdgroup_functions;
		}

		void set_msl_version(uint32_t major, uint32_t minor = 0, uint32_t patch = 0)
		{
			msl_version = make_msl_version(major, minor, patch);
		}

		bool supports_msl_version(uint32_t major, uint32_t minor = 0, uint32_t patch = 0) const
		{
			return msl_version >= make_msl_version(major, minor, patch);
		}

		static uint32_t make_msl_version(uint32_t major, uint32_t minor = 0, uint32_t patch = 0)
		{
			return (major * 10000) + (minor * 100) + patch;
		}
	};

	const Options &get_msl_options() const
	{
		return msl_options;
	}

	void set_msl_options(const Options &opts)
	{
		msl_options = opts;
	}

	// Provide feedback to calling API to allow runtime to disable pipeline
	// rasterization if vertex shader requires rasterization to be disabled.
	bool get_is_rasterization_disabled() const
	{
		return is_rasterization_disabled && (get_entry_point().model == ExecutionModelVertex ||
		                                     get_entry_point().model == ExecutionModelTessellationControl ||
		                                     get_entry_point().model == ExecutionModelTessellationEvaluation);
	}

	// Provide feedback to calling API to allow it to pass an auxiliary
	// swizzle buffer if the shader needs it.
	bool needs_swizzle_buffer() const
	{
		return used_swizzle_buffer;
	}

	// Provide feedback to calling API to allow it to pass a buffer
	// containing STORAGE_BUFFER buffer sizes to support OpArrayLength.
	bool needs_buffer_size_buffer() const
	{
		return !buffers_requiring_array_length.empty();
	}

	bool buffer_requires_array_length(VariableID id) const
	{
		return buffers_requiring_array_length.count(id) != 0;
	}

	// Provide feedback to calling API to allow it to pass a buffer
	// containing the view mask for the current multiview subpass.
	bool needs_view_mask_buffer() const
	{
		return msl_options.multiview && !msl_options.view_index_from_device_index;
	}

	// Provide feedback to calling API to allow it to pass a buffer
	// containing the dispatch base workgroup ID.
	bool needs_dispatch_base_buffer() const
	{
		return msl_options.dispatch_base && !msl_options.supports_msl_version(1, 2);
	}

	// Provide feedback to calling API to allow it to pass an output
	// buffer if the shader needs it.
	bool needs_output_buffer() const
	{
		return capture_output_to_buffer && stage_out_var_id != ID(0);
	}

	// Provide feedback to calling API to allow it to pass a patch output
	// buffer if the shader needs it.
	bool needs_patch_output_buffer() const
	{
		return capture_output_to_buffer && patch_stage_out_var_id != ID(0);
	}

	// Provide feedback to calling API to allow it to pass an input threadgroup
	// buffer if the shader needs it.
	bool needs_input_threadgroup_mem() const
	{
		return capture_output_to_buffer && stage_in_var_id != ID(0);
	}

	explicit CompilerMSL(std::vector<uint32_t> spirv);
	CompilerMSL(const uint32_t *ir, size_t word_count);
	explicit CompilerMSL(const ParsedIR &ir);
	explicit CompilerMSL(ParsedIR &&ir);

	// input is a shader interface variable description used to fix up shader input variables.
	// If shader inputs are provided, is_msl_shader_input_used() will return true after
	// calling ::compile() if the location were used by the MSL code.
	void add_msl_shader_input(const MSLShaderInterfaceVariable &input);

	// output is a shader interface variable description used to fix up shader output variables.
	// If shader outputs are provided, is_msl_shader_output_used() will return true after
	// calling ::compile() if the location were used by the MSL code.
	void add_msl_shader_output(const MSLShaderInterfaceVariable &output);

	// AppGL fork: enumerate the canonical MSL stage-interface struct after
	// compile(). Returns the actual emitted layout — name, location,
	// builtin, type, per-member offset/size, plus the total struct size
	// (padded to alignment) and the struct's alignment. Callers should
	// reflect against this instead of synthesizing a parallel layout from
	// SPIR-V — that mirror would decay silently as the emission patches
	// evolve.
	//
	// storage_class:  StorageClassInput  → main0_in (per-vertex/per-CP) or patch in
	//                 StorageClassOutput → main0_out (per-vertex/per-CP) or patch out
	// patch:          false → per-vertex / per-CP interface (default)
	//                 true  → per-patch interface (TCS patch outputs, TES patch inputs)
	//
	// Returns a layout with empty `members` if compile() has not run, if
	// the requested interface is not present (e.g. patch=true on a stage
	// with no patch interface), or if storage_class is not Input/Output.
	//
	// Asserts (in debug builds) that the running offset after all members
	// is consistent with the computed struct_size, catching future emitter
	// changes that invalidate the introspected offsets.
	MSLInterfaceLayout get_msl_interface_layout(
	    StorageClass storage_class, bool patch = false) const;

	// resource is a resource binding to indicate the MSL buffer,
	// texture or sampler index to use for a particular SPIR-V description set
	// and binding. If resource bindings are provided,
	// is_msl_resource_binding_used() will return true after calling ::compile() if
	// the set/binding combination was used by the MSL code.
	void add_msl_resource_binding(const MSLResourceBinding &resource);

	// desc_set and binding are the SPIR-V descriptor set and binding of a buffer resource
	// in this shader. index is the index within the dynamic offset buffer to use. This
	// function marks that resource as using a dynamic offset (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
	// or VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC). This function only has any effect if argument buffers
	// are enabled. If so, the buffer will have its address adjusted at the beginning of the shader with
	// an offset taken from the dynamic offset buffer.
	void add_dynamic_buffer(uint32_t desc_set, uint32_t binding, uint32_t index);

	// desc_set and binding are the SPIR-V descriptor set and binding of a buffer resource
	// in this shader. This function marks that resource as an inline uniform block
	// (VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT). This function only has any effect if argument buffers
	// are enabled. If so, the buffer block will be directly embedded into the argument
	// buffer, instead of being referenced indirectly via pointer.
	void add_inline_uniform_block(uint32_t desc_set, uint32_t binding);

	// When using MSL argument buffers, we can force "classic" MSL 1.0 binding schemes for certain descriptor sets.
	// This corresponds to VK_KHR_push_descriptor in Vulkan.
	void add_discrete_descriptor_set(uint32_t desc_set);

	// If an argument buffer is large enough, it may need to be in the device storage space rather than
	// constant. Opt-in to this behavior here on a per set basis.
	void set_argument_buffer_device_address_space(uint32_t desc_set, bool device_storage);

	// Query after compilation is done. This allows you to check if an input location was used by the shader.
	bool is_msl_shader_input_used(uint32_t location);

	// Query after compilation is done. This allows you to check if an output location were used by the shader.
	bool is_msl_shader_output_used(uint32_t location);

	// If not using add_msl_shader_input, it's possible
	// that certain builtin attributes need to be automatically assigned locations.
	// This is typical for tessellation builtin inputs such as tess levels, gl_Position, etc.
	// This returns k_unknown_location if the location was explicitly assigned with
	// add_msl_shader_input or the builtin is not used, otherwise returns N in [[attribute(N)]].
	uint32_t get_automatic_builtin_input_location(BuiltIn builtin) const;

	// If not using add_msl_shader_output, it's possible
	// that certain builtin attributes need to be automatically assigned locations.
	// This is typical for tessellation builtin outputs such as tess levels, gl_Position, etc.
	// This returns k_unknown_location if the location were explicitly assigned with
	// add_msl_shader_output or the builtin were not used, otherwise returns N in [[attribute(N)]].
	uint32_t get_automatic_builtin_output_location(BuiltIn builtin) const;

	// NOTE: Only resources which are remapped using add_msl_resource_binding will be reported here.
	// Constexpr samplers are always assumed to be emitted.
	// No specific MSLResourceBinding remapping is required for constexpr samplers as long as they are remapped
	// by remap_constexpr_sampler(_by_binding).
	bool is_msl_resource_binding_used(ExecutionModel model, uint32_t set, uint32_t binding) const;

	// This must only be called after a successful call to CompilerMSL::compile().
	// For a variable resource ID obtained through reflection API, report the automatically assigned resource index.
	// If the descriptor set was part of an argument buffer, report the [[id(N)]],
	// or [[buffer/texture/sampler]] binding for other resources.
	// If the resource was a combined image sampler, report the image binding here,
	// use the _secondary version of this call to query the sampler half of the resource.
	// If no binding exists, uint32_t(-1) is returned.
	uint32_t get_automatic_msl_resource_binding(uint32_t id) const;

	// Same as get_automatic_msl_resource_binding, but should only be used for combined image samplers, in which case the
	// sampler's binding is returned instead. For any other resource type, -1 is returned.
	// Secondary bindings are also used for the auxillary image atomic buffer.
	uint32_t get_automatic_msl_resource_binding_secondary(uint32_t id) const;

	// Same as get_automatic_msl_resource_binding, but should only be used for combined image samplers for multiplanar images,
	// in which case the second plane's binding is returned instead. For any other resource type, -1 is returned.
	uint32_t get_automatic_msl_resource_binding_tertiary(uint32_t id) const;

	// Same as get_automatic_msl_resource_binding, but should only be used for combined image samplers for triplanar images,
	// in which case the third plane's binding is returned instead. For any other resource type, -1 is returned.
	uint32_t get_automatic_msl_resource_binding_quaternary(uint32_t id) const;

	// Compiles the SPIR-V code into Metal Shading Language.
	std::string compile() override;

	// Remap a sampler with ID to a constexpr sampler.
	// Older iOS targets must use constexpr samplers in certain cases (PCF),
	// so a static sampler must be used.
	// The sampler will not consume a binding, but be declared in the entry point as a constexpr sampler.
	// This can be used on both combined image/samplers (sampler2D) or standalone samplers.
	// The remapped sampler must not be an array of samplers.
	// Prefer remap_constexpr_sampler_by_binding unless you're also doing reflection anyways.
	void remap_constexpr_sampler(VariableID id, const MSLConstexprSampler &sampler);

	// Same as remap_constexpr_sampler, except you provide set/binding, rather than variable ID.
	// Remaps based on ID take priority over set/binding remaps.
	void remap_constexpr_sampler_by_binding(uint32_t desc_set, uint32_t binding, const MSLConstexprSampler &sampler);

	// If using CompilerMSL::Options::pad_fragment_output_components, override the number of components we expect
	// to use for a particular location. The default is 4 if number of components is not overridden.
	void set_fragment_output_components(uint32_t location, uint32_t components);

	void set_combined_sampler_suffix(const char *suffix);
	const char *get_combined_sampler_suffix() const;

	// Information about specialization constants that are translated into MSL macros
	// instead of using function constant
	// These must only be called after a successful call to CompilerMSL::compile().
	bool specialization_constant_is_macro(uint32_t constant_id) const;

	// Returns a mask of SPIR-V FP Fast Math Mode flags, that represents the set of flags that can be applied
	// across all floating-point types. Each FPFastMathDefault execution mode operation identifies the flags
	// for one floating-point type, and the value returned here is a bitwise-AND combination across all types.
	// If incl_ops is enabled, the FPFastMathMode of any SPIR-V operations are also included in the bitwise-AND
	// to determine the minimal fast-math that applies to all default execution modes and all operations.
	// The returned value is also affected by execution modes SignedZeroInfNanPreserve and ContractionOff.
	uint32_t get_fp_fast_math_flags(bool incl_ops) const;

protected:
	// An enum of SPIR-V functions that are implemented in additional
	// source code that is added to the shader if necessary.
	enum SPVFuncImpl : uint8_t
	{
		SPVFuncImplNone,
		SPVFuncImplMod,
		SPVFuncImplSMod,
		SPVFuncImplRadians,
		SPVFuncImplDegrees,
		SPVFuncImplFindILsb,
		SPVFuncImplFindSMsb,
		SPVFuncImplFindUMsb,
		SPVFuncImplSSign,
		SPVFuncImplArrayCopy,
		SPVFuncImplArrayCopyMultidim,
		SPVFuncImplArrayCopyExtendedSrc,
		SPVFuncImplArrayCopyExtendedDst,
		SPVFuncImplTexelBufferCoords,
		SPVFuncImplImage2DAtomicCoords, // Emulate texture2D atomic operations
		SPVFuncImplGradientCube,
		SPVFuncImplFMul,
		SPVFuncImplFAdd,
		SPVFuncImplFSub,
		SPVFuncImplQuantizeToF16,
		SPVFuncImplCubemapTo2DArrayFace,
		SPVFuncImplUnsafeArray, // Allow Metal to use the array<T> template to make arrays a value type
		SPVFuncImplStorageMatrix, // Allow threadgroup construction of matrices
		SPVFuncImplInverse4x4,
		SPVFuncImplInverse3x3,
		SPVFuncImplInverse2x2,
		// It is very important that this come before *Swizzle, to ensure it's emitted before them.
		SPVFuncImplGetSwizzle,
		SPVFuncImplTextureSwizzle,
		SPVFuncImplGatherReturn,
		SPVFuncImplGatherCompareReturn,
		SPVFuncImplGatherSwizzle,
		SPVFuncImplGatherCompareSwizzle,
		SPVFuncImplGatherConstOffsets,
		SPVFuncImplGatherCompareConstOffsets,
		SPVFuncImplGatherRect,
		SPVFuncImplGatherRectCompare,
		SPVFuncImplGatherRectCompareConstOffsets,
		SPVFuncImplSubgroupBroadcast,
		SPVFuncImplSubgroupBroadcastFirst,
		SPVFuncImplSubgroupBallot,
		SPVFuncImplSubgroupBallotBitExtract,
		SPVFuncImplSubgroupBallotFindLSB,
		SPVFuncImplSubgroupBallotFindMSB,
		SPVFuncImplSubgroupBallotBitCount,
		SPVFuncImplSubgroupAllEqual,
		SPVFuncImplSubgroupShuffle,
		SPVFuncImplSubgroupShuffleXor,
		SPVFuncImplSubgroupShuffleUp,
		SPVFuncImplSubgroupShuffleDown,
		SPVFuncImplSubgroupRotate,
		SPVFuncImplSubgroupClusteredAdd,
		SPVFuncImplSubgroupClusteredFAdd = SPVFuncImplSubgroupClusteredAdd,
		SPVFuncImplSubgroupClusteredIAdd = SPVFuncImplSubgroupClusteredAdd,
		SPVFuncImplSubgroupClusteredMul,
		SPVFuncImplSubgroupClusteredFMul = SPVFuncImplSubgroupClusteredMul,
		SPVFuncImplSubgroupClusteredIMul = SPVFuncImplSubgroupClusteredMul,
		SPVFuncImplSubgroupClusteredMin,
		SPVFuncImplSubgroupClusteredFMin = SPVFuncImplSubgroupClusteredMin,
		SPVFuncImplSubgroupClusteredSMin = SPVFuncImplSubgroupClusteredMin,
		SPVFuncImplSubgroupClusteredUMin = SPVFuncImplSubgroupClusteredMin,
		SPVFuncImplSubgroupClusteredMax,
		SPVFuncImplSubgroupClusteredFMax = SPVFuncImplSubgroupClusteredMax,
		SPVFuncImplSubgroupClusteredSMax = SPVFuncImplSubgroupClusteredMax,
		SPVFuncImplSubgroupClusteredUMax = SPVFuncImplSubgroupClusteredMax,
		SPVFuncImplSubgroupClusteredAnd,
		SPVFuncImplSubgroupClusteredBitwiseAnd = SPVFuncImplSubgroupClusteredAnd,
		SPVFuncImplSubgroupClusteredLogicalAnd = SPVFuncImplSubgroupClusteredAnd,
		SPVFuncImplSubgroupClusteredOr,
		SPVFuncImplSubgroupClusteredBitwiseOr = SPVFuncImplSubgroupClusteredOr,
		SPVFuncImplSubgroupClusteredLogicalOr = SPVFuncImplSubgroupClusteredOr,
		SPVFuncImplSubgroupClusteredXor,
		SPVFuncImplSubgroupClusteredBitwiseXor = SPVFuncImplSubgroupClusteredXor,
		SPVFuncImplSubgroupClusteredLogicalXor = SPVFuncImplSubgroupClusteredXor,
		SPVFuncImplQuadBroadcast,
		SPVFuncImplQuadSwap,
		SPVFuncImplReflectScalar,
		SPVFuncImplRefractScalar,
		SPVFuncImplFaceForwardScalar,
		SPVFuncImplChromaReconstructNearest2Plane,
		SPVFuncImplChromaReconstructNearest3Plane,
		SPVFuncImplChromaReconstructLinear422CositedEven2Plane,
		SPVFuncImplChromaReconstructLinear422CositedEven3Plane,
		SPVFuncImplChromaReconstructLinear422Midpoint2Plane,
		SPVFuncImplChromaReconstructLinear422Midpoint3Plane,
		SPVFuncImplChromaReconstructLinear420XCositedEvenYCositedEven2Plane,
		SPVFuncImplChromaReconstructLinear420XCositedEvenYCositedEven3Plane,
		SPVFuncImplChromaReconstructLinear420XMidpointYCositedEven2Plane,
		SPVFuncImplChromaReconstructLinear420XMidpointYCositedEven3Plane,
		SPVFuncImplChromaReconstructLinear420XCositedEvenYMidpoint2Plane,
		SPVFuncImplChromaReconstructLinear420XCositedEvenYMidpoint3Plane,
		SPVFuncImplChromaReconstructLinear420XMidpointYMidpoint2Plane,
		SPVFuncImplChromaReconstructLinear420XMidpointYMidpoint3Plane,
		SPVFuncImplExpandITUFullRange,
		SPVFuncImplExpandITUNarrowRange,
		SPVFuncImplConvertYCbCrBT709,
		SPVFuncImplConvertYCbCrBT601,
		SPVFuncImplConvertYCbCrBT2020,
		SPVFuncImplDynamicImageSampler,
		SPVFuncImplRayQueryIntersectionParams,
		SPVFuncImplVariableDescriptor,
		SPVFuncImplVariableSizedDescriptor,
		SPVFuncImplVariableDescriptorArray,
		SPVFuncImplPaddedStd140,
		SPVFuncImplPaddedArrayElement,
		SPVFuncImplReduceAdd,
		SPVFuncImplImageFence,
		SPVFuncImplTextureCast,
		SPVFuncImplMulExtended,
		SPVFuncImplSetMeshOutputsEXT,
		SPVFuncImplAssume,
	};

	// If the underlying resource has been used for comparison then duplicate loads of that resource must be too
	// Use Metal's native frame-buffer fetch API for subpass inputs.
	void emit_texture_op(const Instruction &i, bool sparse) override;
	void emit_binary_ptr_op(uint32_t result_type, uint32_t result_id, uint32_t op0, uint32_t op1, const char *op);
	std::string to_ptr_expression(uint32_t id, bool register_expression_read = true);
	void emit_binary_unord_op(uint32_t result_type, uint32_t result_id, uint32_t op0, uint32_t op1, const char *op);
	void emit_instruction(const Instruction &instr) override;
	void emit_glsl_op(uint32_t result_type, uint32_t result_id, uint32_t op, const uint32_t *args,
	                  uint32_t count) override;
	void emit_spv_amd_shader_trinary_minmax_op(uint32_t result_type, uint32_t result_id, uint32_t op,
	                                           const uint32_t *args, uint32_t count) override;
	void emit_header() override;
	void emit_appgl_fp64_emulation_helpers();
	void emit_function_prototype(SPIRFunction &func, const Bitset &return_flags) override;
	void emit_sampled_image_op(uint32_t result_type, uint32_t result_id, uint32_t image_id, uint32_t samp_id) override;
	void emit_subgroup_op(const Instruction &i) override;
	void emit_subgroup_cluster_op(uint32_t result_type, uint32_t result_id, uint32_t cluster_size, uint32_t op0,
	                              const char *op);
	void emit_subgroup_cluster_op_cast(uint32_t result_type, uint32_t result_id, uint32_t cluster_size, uint32_t op0,
	                                   const char *op, SPIRType::BaseType input_type,
	                                   SPIRType::BaseType expected_result_type);
	std::string to_texture_op(const Instruction &i, bool sparse, bool *forward,
	                          SmallVector<uint32_t> &inherited_expressions) override;
	void emit_fixup() override;
	std::string to_struct_member(const SPIRType &type, uint32_t member_type_id, uint32_t index,
	                             const std::string &qualifier = "");
	void emit_struct_member(const SPIRType &type, uint32_t member_type_id, uint32_t index,
	                        const std::string &qualifier = "", uint32_t base_offset = 0) override;
	std::string type_to_glsl(const SPIRType &type, uint32_t id, bool member);
	std::string type_to_glsl(const SPIRType &type, uint32_t id = 0) override;
	void emit_block_hints(const SPIRBlock &block) override;
	void emit_mesh_entry_point();
	void emit_mesh_outputs();
	void emit_mesh_tasks(SPIRBlock &block) override;
	void emit_workgroup_initialization(const SPIRVariable &var) override;

	// Allow Metal to use the array<T> template to make arrays a value type
	std::string type_to_array_glsl(const SPIRType &type, uint32_t variable_id) override;
	std::string constant_op_expression(const SPIRConstantOp &cop) override;
	std::string constant_expression_vector(const SPIRConstant &c, uint32_t vector) override;

	bool variable_decl_is_remapped_storage(const SPIRVariable &variable, StorageClass storage) const override;

	// GCC workaround of lambdas calling protected functions (for older GCC versions)
	std::string variable_decl(const SPIRType &type, const std::string &name, uint32_t id = 0) override;

	std::string image_type_glsl(const SPIRType &type, uint32_t id, bool member) override;
	std::string sampler_type(const SPIRType &type, uint32_t id, bool member);
	std::string builtin_to_glsl(BuiltIn builtin, StorageClass storage) override;
	std::string to_func_call_arg(const SPIRFunction::Parameter &arg, uint32_t id) override;
	std::string to_name(uint32_t id, bool allow_alias = true) const override;
	std::string to_function_name(const TextureFunctionNameArguments &args) override;
	std::string to_function_args(const TextureFunctionArguments &args, bool *p_forward) override;
	std::string to_initializer_expression(const SPIRVariable &var) override;
	std::string to_zero_initialized_expression(uint32_t type_id) override;

	std::string unpack_expression_type(std::string expr_str, const SPIRType &type, uint32_t physical_type_id,
	                                   bool is_packed, bool row_major) override;

	bool appgl_fp64_emulation_enabled_for_type(const SPIRType &type) const;
	bool appgl_fp64_emulation_enabled_for_type_id(uint32_t type_id) const;
	bool appgl_fp64_module_uses_emulation() const;
	std::string appgl_fp64_msl_type_name(const SPIRType &type) const;
	std::string appgl_fp64_constant_component(const SPIRConstant &c, uint32_t col, uint32_t row);
	std::string appgl_fp64_float_literal(float value) const;

	// Returns true for BuiltInSampleMask because gl_SampleMask[] is an array in SPIR-V, but [[sample_mask]] is a scalar in Metal.
	bool builtin_translates_to_nonarray(BuiltIn builtin) const override;

	std::string bitcast_glsl_op(const SPIRType &result_type, const SPIRType &argument_type) override;
	bool emit_complex_bitcast(uint32_t result_id, uint32_t id, uint32_t op0) override;
	bool skip_argument(uint32_t id) const override;
	std::string to_member_reference(uint32_t base, const SPIRType &type, uint32_t index, bool ptr_chain_is_resolved) override;
	std::string to_qualifiers_glsl(uint32_t id) override;
	void replace_illegal_names() override;
	void declare_constant_arrays();

	void replace_illegal_entry_point_names();
	void sync_entry_point_aliases_and_names();

	static const std::unordered_set<std::string> &get_reserved_keyword_set();
	static const std::unordered_set<std::string> &get_illegal_func_names();

	// Constant arrays of non-primitive types (i.e. matrices) won't link properly into Metal libraries
	void declare_complex_constant_arrays();

	bool is_patch_block(const SPIRType &type);
	bool is_non_native_row_major_matrix(uint32_t id) override;
	bool member_is_non_native_row_major_matrix(const SPIRType &type, uint32_t index) override;
	std::string convert_row_major_matrix(std::string exp_str, const SPIRType &exp_type, uint32_t physical_type_id,
	                                     bool is_packed, bool relaxed) override;

	bool is_tesc_shader() const;
	bool is_tese_shader() const;
	bool is_mesh_shader() const;

	void preprocess_op_codes();
	void localize_global_variables();
	void extract_global_variables_from_functions();
	void mark_packable_structs();
	void mark_as_packable(SPIRType &type);
	void mark_as_workgroup_struct(SPIRType &type);

	std::unordered_map<uint32_t, std::set<uint32_t>> function_global_vars;
	void extract_global_variables_from_function(uint32_t func_id, std::set<uint32_t> &added_arg_ids,
	                                            std::unordered_set<uint32_t> &global_var_ids,
	                                            std::unordered_set<uint32_t> &processed_func_ids);
	uint32_t add_interface_block(StorageClass storage, bool patch = false);
	uint32_t add_interface_block_pointer(uint32_t ib_var_id, StorageClass storage);
	uint32_t add_meshlet_block(bool per_primitive);

	// Path A++ gate 7 (msl-geometry-shader-as-mesh.patch): shared-struct
	// layout parity. Walks the GS's input gl_PerVertex block in IR,
	// finds gl_ClipDistance / gl_CullDistance members the existing
	// add_interface_block flow dropped, and injects them into main0_in
	// so its byte layout matches the linked-VS main0_out emission.
	void ensure_gs_as_mesh_gl_per_vertex_parity();

	// Phase 2.5 Gap B (msl-geometry-shader-as-mesh.patch): emits the
	// EndPrimitive flush sequence for the GS-as-mesh path. Shared
	// between explicit `OpEndPrimitive` and Path H's implicit
	// function-exit flush. Branches on output topology +
	// max_vertices:
	//   - triangle_strip with max_vertices ≤ 3: 1-triangle simple emit
	//     (preserves byte-identical output for the synthetic / Phase 2
	//     MVP envelope).
	//   - triangle_strip with max_vertices > 3: strip-to-list expansion
	//     loop with even/odd winding alternation per GL §10.1.13.
	//   - line_strip with max_vertices ≤ 2: 1-line simple emit.
	//   - line_strip with max_vertices > 2: (N-1)-line expansion loop.
	//   - points: 1-point emit (max_vertices irrelevant).
	void emit_gs_as_mesh_endprimitive();

	// Path J' Option E (Sprint 6 — Class 2A spec-compliance): walks
	// SPIR-V Input variables at compile() preamble, populates
	// `inputs_by_location` with each natural Input variable's name +
	// location + component, then dedupes any synthetic-range entry
	// (location >= 0xE0000000) whose name matches a natural-range
	// entry. Coordinates with Path J' Option A1's add-time dedupe
	// (fce60f8) — Option A1 fires when consumers call
	// `add_msl_shader_input` AFTER compile() pre-population has run;
	// Option E catches the typical case where consumers register
	// synthetic-range entries BEFORE compile() (the order Option A1's
	// add-time check can't see naturally-emitted Input variables yet).
	// The combined effect: `inputs_by_location` is canonical for both
	// API-registered and natural-emission entries by the time
	// `add_interface_block(StorageClassInput)` runs.
	void pre_populate_inputs_by_location_from_ir();

	// Phase 2.5 Gap A (msl-geometry-shader-as-mesh.patch): synthesizes
	// a `gl_Position` Output variable in IR for GS-as-mesh shaders that
	// don't write one. Apple Metal mesh shader vertex types must declare
	// a `[[position]]` member; GLSL geometry shaders that omit
	// `gl_Position = ...` (TF-only / depth-only patterns) violate this
	// requirement. Synthesizing a vec4 Output with BuiltInPosition
	// decoration carries through the existing add_meshlet_block /
	// spvPerVertex emission machinery without further changes — the
	// body never writes it, the function-local `spvVertices = {}`
	// zero-init carries the default value (0,0,0,0) through to
	// set_vertex.
	void ensure_gs_as_mesh_position_output();


	struct InterfaceBlockMeta
	{
		struct LocationMeta
		{
			uint32_t base_type_id = 0;
			uint32_t num_components = 0;
			bool flat = false;
			bool noperspective = false;
			bool centroid = false;
			bool sample = false;
		};
		std::unordered_map<uint32_t, LocationMeta> location_meta;
		bool strip_array = false;
		bool allow_local_declaration = false;
	};

	std::string to_tesc_invocation_id();
	void emit_local_masked_variable(const SPIRVariable &masked_var, bool strip_array);
	void add_variable_to_interface_block(StorageClass storage, const std::string &ib_var_ref, SPIRType &ib_type,
	                                     SPIRVariable &var, InterfaceBlockMeta &meta);
	void add_composite_variable_to_interface_block(StorageClass storage, const std::string &ib_var_ref,
	                                               SPIRType &ib_type, SPIRVariable &var, InterfaceBlockMeta &meta);
	void add_plain_variable_to_interface_block(StorageClass storage, const std::string &ib_var_ref,
	                                           SPIRType &ib_type, SPIRVariable &var, InterfaceBlockMeta &meta);
	bool add_component_variable_to_interface_block(StorageClass storage, const std::string &ib_var_ref,
	                                               SPIRVariable &var, const SPIRType &type,
	                                               InterfaceBlockMeta &meta);
	void add_plain_member_variable_to_interface_block(StorageClass storage,
	                                                  const std::string &ib_var_ref, SPIRType &ib_type,
	                                                  SPIRVariable &var, SPIRType &var_type,
	                                                  uint32_t mbr_idx, InterfaceBlockMeta &meta,
	                                                  const std::string &mbr_name_qual,
	                                                  const std::string &var_chain_qual,
	                                                  uint32_t &location, uint32_t &var_mbr_idx);
	void add_composite_member_variable_to_interface_block(StorageClass storage,
	                                                      const std::string &ib_var_ref, SPIRType &ib_type,
	                                                      SPIRVariable &var, SPIRType &var_type,
	                                                      uint32_t mbr_idx, InterfaceBlockMeta &meta,
	                                                      const std::string &mbr_name_qual,
	                                                      const std::string &var_chain_qual,
	                                                      uint32_t &location, uint32_t &var_mbr_idx,
	                                                      const Bitset &interpolation_qual);
	void add_tess_level_input_to_interface_block(const std::string &ib_var_ref, SPIRType &ib_type, SPIRVariable &var);
	void add_tess_level_input(const std::string &base_ref, const std::string &mbr_name, SPIRVariable &var);

	void ensure_struct_members_valid_vecsizes(SPIRType &struct_type, uint32_t &location);
	void fix_up_interface_member_indices(StorageClass storage, uint32_t ib_type_id);

	void mark_location_as_used_by_shader(uint32_t location, const SPIRType &type,
	                                     StorageClass storage, bool fallback = false);
	uint32_t ensure_correct_builtin_type(uint32_t type_id, BuiltIn builtin);
	uint32_t ensure_correct_input_type(uint32_t type_id, uint32_t location, uint32_t component,
	                                   uint32_t num_components, bool strip_array);

	void emit_custom_templates();
	void emit_custom_functions();
	void emit_resources();
	void emit_specialization_constants_and_structs();
	void emit_interface_block(uint32_t ib_var_id);
	bool maybe_emit_array_assignment(uint32_t id_lhs, uint32_t id_rhs);
	bool is_var_runtime_size_array(const SPIRVariable &var) const;
	uint32_t get_resource_array_size(const SPIRType &type, uint32_t id) const;

	void fix_up_shader_inputs_outputs();

	bool entry_point_is_vertex() const;
	bool entry_point_returns_stage_output() const;
	bool entry_point_requires_const_device_buffers() const;
	std::string func_type_decl(SPIRType &type);
	std::string entry_point_args_classic(bool append_comma);
	std::string entry_point_args_argument_buffer(bool append_comma);
	std::string entry_point_arg_stage_in();
	void entry_point_args_builtin(std::string &args);
	void entry_point_args_discrete_descriptors(std::string &args);
	std::string append_member_name(const std::string &qualifier, const SPIRType &type, uint32_t index);
	std::string ensure_valid_name(std::string name, std::string pfx);
	std::string to_sampler_expression(uint32_t id);
	std::string to_swizzle_expression(uint32_t id);
	std::string to_buffer_size_expression(uint32_t id);
	bool is_sample_rate() const;
	bool is_intersection_query() const;
	bool is_direct_input_builtin(BuiltIn builtin);
	std::string builtin_qualifier(BuiltIn builtin);
	std::string builtin_type_decl(BuiltIn builtin, uint32_t id = 0);
	std::string built_in_func_arg(BuiltIn builtin, bool prefix_comma);
	std::string member_attribute_qualifier(const SPIRType &type, uint32_t index);
	std::string member_location_attribute_qualifier(const SPIRType &type, uint32_t index);
	std::string argument_decl(const SPIRFunction::Parameter &arg);
	const char *descriptor_address_space(uint32_t id, StorageClass storage, const char *plain_address_space) const;
	std::string round_fp_tex_coords(std::string tex_coords, bool coord_is_fp);
	uint32_t get_metal_resource_index(SPIRVariable &var, SPIRType::BaseType basetype, uint32_t plane = 0);
	uint32_t get_member_location(uint32_t type_id, uint32_t index, uint32_t *comp = nullptr) const;
	uint32_t get_or_allocate_builtin_input_member_location(BuiltIn builtin,
	                                                       uint32_t type_id, uint32_t index, uint32_t *comp = nullptr);
	uint32_t get_or_allocate_builtin_output_member_location(BuiltIn builtin,
	                                                        uint32_t type_id, uint32_t index, uint32_t *comp = nullptr);

	uint32_t get_physical_tess_level_array_size(BuiltIn builtin) const;

	uint32_t get_physical_type_id_stride(TypeID type_id) const override;

	// MSL packing rules. These compute the effective packing rules as observed by the MSL compiler in the MSL output.
	// These values can change depending on various extended decorations which control packing rules.
	// We need to make these rules match up with SPIR-V declared rules.
	uint32_t get_declared_type_size_msl(TypeID type_id, const SPIRType *special_type, bool packed, bool row_major) const;
	uint32_t get_declared_type_array_stride_msl(TypeID type_id, const SPIRType *special_type, bool packed, bool row_major) const;
	uint32_t get_declared_type_matrix_stride_msl(TypeID type_id, const SPIRType *special_type, bool packed, bool row_major) const;
	uint32_t get_declared_type_alignment_msl(TypeID type_id, const SPIRType *special_type, bool packed, bool row_major) const;

	uint32_t get_declared_struct_member_size_msl(const SPIRType &struct_type, uint32_t index) const;
	uint32_t get_declared_struct_member_array_stride_msl(const SPIRType &struct_type, uint32_t index) const;
	uint32_t get_declared_struct_member_matrix_stride_msl(const SPIRType &struct_type, uint32_t index) const;
	uint32_t get_declared_struct_member_alignment_msl(const SPIRType &struct_type, uint32_t index) const;

	uint32_t get_declared_input_size_msl(const SPIRType &struct_type, uint32_t index) const;
	uint32_t get_declared_input_array_stride_msl(const SPIRType &struct_type, uint32_t index) const;
	uint32_t get_declared_input_matrix_stride_msl(const SPIRType &struct_type, uint32_t index) const;
	uint32_t get_declared_input_alignment_msl(const SPIRType &struct_type, uint32_t index) const;

	TypeID get_physical_member_type_id(const SPIRType &struct_type, uint32_t index) const;
	SPIRType get_presumed_input_type(const SPIRType &struct_type, uint32_t index) const;

	uint32_t get_declared_struct_size_msl(const SPIRType &struct_type) const;

	std::string to_component_argument(uint32_t id);
	void align_struct(SPIRType &ib_type, std::unordered_set<uint32_t> &aligned_structs);
	void mark_scalar_layout_structs(const SPIRType &ib_type);
	void mark_struct_members_packed(const SPIRType &type);
	void ensure_member_packing_rules_msl(SPIRType &ib_type, uint32_t index);
	bool validate_member_packing_rules_msl(const SPIRType &type, uint32_t index) const;
	std::string get_variable_address_space(const SPIRVariable &argument);
	// Special case of get_variable_address_space which is only used for leaf functions.
	std::string get_leaf_argument_address_space(const SPIRVariable &argument);
	std::string get_type_address_space(const SPIRType &type, uint32_t id, bool argument = false);
	bool decoration_flags_signal_volatile(const Bitset &flags) const;
	bool decoration_flags_signal_coherent(const Bitset &flags) const;
	const char *to_restrict(uint32_t id, bool space);
	SPIRType &get_stage_in_struct_type();
	SPIRType &get_stage_out_struct_type();
	SPIRType &get_patch_stage_in_struct_type();
	SPIRType &get_patch_stage_out_struct_type();
	std::string get_tess_factor_struct_name();
	SPIRType &get_uint_type();
	uint32_t get_uint_type_id();
	void emit_atomic_func_op(uint32_t result_type, uint32_t result_id, const char *op, Op opcode,
	                         uint32_t mem_order_1, uint32_t mem_order_2, bool has_mem_order_2, uint32_t op0, uint32_t op1 = 0,
	                         bool op1_is_pointer = false, bool op1_is_literal = false, uint32_t op2 = 0);
	const char *get_memory_order(uint32_t spv_mem_sem);
	void add_pragma_line(const std::string &line, bool recompile_on_unique);
	void add_typedef_line(const std::string &line);
	void emit_barrier(uint32_t id_exe_scope, uint32_t id_mem_scope, uint32_t id_mem_sem);
	bool emit_array_copy(const char *expr, uint32_t lhs_id, uint32_t rhs_id,
	                     StorageClass lhs_storage, StorageClass rhs_storage) override;
	void build_implicit_builtins();
	uint32_t build_constant_uint_array_pointer();
	void emit_entry_point_declarations() override;
	bool uses_explicit_early_fragment_test();

	uint32_t builtin_frag_coord_id = 0;
	uint32_t builtin_sample_id_id = 0;
	uint32_t builtin_sample_mask_id = 0;
	uint32_t builtin_helper_invocation_id = 0;
	uint32_t builtin_vertex_idx_id = 0;
	uint32_t builtin_base_vertex_id = 0;
	uint32_t builtin_instance_idx_id = 0;
	uint32_t builtin_base_instance_id = 0;
	uint32_t builtin_view_idx_id = 0;
	uint32_t builtin_layer_id = 0;
	uint32_t builtin_invocation_id_id = 0;
	uint32_t builtin_primitive_id_id = 0;
	uint32_t builtin_subgroup_invocation_id_id = 0;
	uint32_t builtin_subgroup_size_id = 0;
	uint32_t builtin_dispatch_base_id = 0;
	uint32_t builtin_stage_input_size_id = 0;
	uint32_t builtin_local_invocation_index_id = 0;
	uint32_t builtin_workgroup_size_id = 0;
	uint32_t builtin_mesh_primitive_indices_id = 0;
	uint32_t builtin_mesh_sizes_id = 0;
	uint32_t builtin_task_grid_id = 0;
	uint32_t builtin_frag_depth_id = 0;
	uint32_t swizzle_buffer_id = 0;
	uint32_t buffer_size_buffer_id = 0;
	uint32_t view_mask_buffer_id = 0;
	uint32_t dynamic_offsets_buffer_id = 0;
	uint32_t uint_type_id = 0;
	uint32_t shared_uint_type_id = 0;
	uint32_t meshlet_type_id = 0;
	uint32_t argument_buffer_padding_buffer_type_id = 0;
	uint32_t argument_buffer_padding_image_type_id = 0;
	uint32_t argument_buffer_padding_sampler_type_id = 0;

	bool does_shader_write_sample_mask = false;
	bool frag_shader_needs_discard_checks = false;

	void cast_to_variable_store(uint32_t target_id, std::string &expr, const SPIRType &expr_type) override;
	void cast_from_variable_load(uint32_t source_id, std::string &expr, const SPIRType &expr_type) override;
	void emit_store_statement(uint32_t lhs_expression, uint32_t rhs_expression) override;

	void analyze_sampled_image_usage();
	void analyze_workgroup_variables();

	bool access_chain_needs_stage_io_builtin_translation(uint32_t base) override;
	bool prepare_access_chain_for_scalar_access(std::string &expr, const SPIRType &type, StorageClass storage,
	                                            bool &is_packed) override;
	void fix_up_interpolant_access_chain(const uint32_t *ops, uint32_t length);
	bool check_physical_type_cast(std::string &expr, const SPIRType *type, uint32_t physical_type) override;

	bool emit_tessellation_access_chain(const uint32_t *ops, uint32_t length);
	bool emit_tessellation_io_load(uint32_t result_type, uint32_t id, uint32_t ptr);
	bool is_out_of_bounds_tessellation_level(uint32_t id_lhs);

	void ensure_builtin(StorageClass storage, BuiltIn builtin);

	void mark_implicit_builtin(StorageClass storage, BuiltIn builtin, uint32_t id);

	std::string convert_to_f32(const std::string &expr, uint32_t components);

	Options msl_options;
	std::set<SPVFuncImpl> spv_function_implementations;
	// Must be ordered to ensure declarations are in a specific order.
	std::map<LocationComponentPair, MSLShaderInterfaceVariable> inputs_by_location;
	// Path J' Option E.3 (Sprint 6 Phase 1 — paired Class 2A + 2C):
	// names recorded at `add_msl_shader_input` call time for synthetic-
	// range entries (location >= 0xE0000000), in call-arrival order. The
	// list captures the orchestrator's intended cross-stage emission
	// sequence (e.g., a TCS-out struct field order that TES-in must
	// match for cross-stage byte alignment). When non-empty, the
	// `add_interface_block` IR walk and supplementation walk both
	// reorder their per-member emission to honor this list, replacing
	// the default IR-ID order / map-key order. Empty by default —
	// non-AppGL consumers retain unchanged emission ordering.
	//
	// Class 2A side: preamble-time uniform record (every synthetic-
	// range `add_msl_shader_input` call appends, regardless of whether
	// the entry survives Path J' Option A1's add-time dedupe — the
	// orchestrator's intent is "emit this name at this position" even
	// if the synthetic-keyed map entry itself gets deduped against a
	// natural-loc entry).
	//
	// Class 2C side: emission-time gate fires only when the list is
	// non-empty (range-gated on synthetic-match having actually
	// happened). Default-empty consumers see no behavior change.
	std::vector<std::string> inputs_by_location_insertion_order;
	std::unordered_map<uint32_t, MSLShaderInterfaceVariable> inputs_by_builtin;
	std::map<LocationComponentPair, MSLShaderInterfaceVariable> outputs_by_location;
	std::unordered_map<uint32_t, MSLShaderInterfaceVariable> outputs_by_builtin;
	std::unordered_set<uint32_t> location_inputs_in_use;
	std::unordered_set<uint32_t> location_inputs_in_use_fallback;
	std::unordered_set<uint32_t> location_outputs_in_use;
	std::unordered_set<uint32_t> location_outputs_in_use_fallback;
	std::unordered_map<uint32_t, uint32_t> fragment_output_components;
	std::unordered_map<uint32_t, uint32_t> builtin_to_automatic_input_location;
	std::unordered_map<uint32_t, uint32_t> builtin_to_automatic_output_location;
	std::vector<std::string> pragma_lines;
	std::vector<std::string> typedef_lines;
	SmallVector<uint32_t> vars_needing_early_declaration;
	std::unordered_set<uint32_t> constant_macro_ids;

	std::unordered_map<StageSetBinding, std::pair<MSLResourceBinding, bool>, InternalHasher> resource_bindings;
	std::unordered_map<StageSetBinding, uint32_t, InternalHasher> resource_arg_buff_idx_to_binding_number;

	uint32_t next_metal_resource_index_buffer = 0;
	uint32_t next_metal_resource_index_texture = 0;
	uint32_t next_metal_resource_index_sampler = 0;
	// Intentionally uninitialized, works around MSVC 2013 bug.
	uint32_t next_metal_resource_ids[kMaxArgumentBuffers];

	VariableID stage_in_var_id = 0;
	VariableID stage_out_var_id = 0;
	VariableID patch_stage_in_var_id = 0;
	VariableID patch_stage_out_var_id = 0;
	VariableID stage_in_ptr_var_id = 0;
	VariableID stage_out_ptr_var_id = 0;
	VariableID tess_level_inner_var_id = 0;
	VariableID tess_level_outer_var_id = 0;
	VariableID mesh_out_per_vertex = 0;
	VariableID mesh_out_per_primitive = 0;
	VariableID stage_out_masked_builtin_type_id = 0;

	// Handle HLSL-style 0-based vertex/instance index.
	enum class TriState
	{
		Neutral,
		No,
		Yes
	};
	TriState needs_base_vertex_arg = TriState::Neutral;
	TriState needs_base_instance_arg = TriState::Neutral;

	bool has_sampled_images = false;
	bool builtin_declaration = false; // Handle HLSL-style 0-based vertex/instance index.

	bool is_using_builtin_array = false; // Force the use of C style array declaration.
	bool using_builtin_array() const;

	bool is_rasterization_disabled = false;
	bool has_descriptor_side_effects_buffer = false;
	bool capture_output_to_buffer = false;
	bool needs_swizzle_buffer_def = false;
	bool used_swizzle_buffer = false;
	bool added_builtin_tess_level = false;
	bool needs_local_invocation_index = false;
	bool needs_subgroup_invocation_id = false;
	bool needs_subgroup_size = false;
	bool needs_sample_id = false;
	bool needs_helper_invocation = false;
	bool needs_workgroup_zero_init = false;
	bool needs_point_size_output = false;
	bool writes_to_depth = false;
	bool writes_to_point_size = false;
	std::string qual_pos_var_name;
	std::string stage_in_var_name = "in";
	std::string stage_out_var_name = "out";
	std::string patch_stage_in_var_name = "patchIn";
	std::string patch_stage_out_var_name = "patchOut";
	std::string sampler_name_suffix = "Smplr";
	std::string swizzle_name_suffix = "Swzl";
	std::string buffer_size_name_suffix = "BufferSize";
	std::string plane_name_suffix = "Plane";
	std::string input_wg_var_name = "gl_in";
	std::string input_buffer_var_name = "spvIn";
	std::string output_buffer_var_name = "spvOut";
	std::string patch_input_buffer_var_name = "spvPatchIn";
	std::string patch_output_buffer_var_name = "spvPatchOut";
	std::string tess_factor_buffer_var_name = "spvTessLevel";
	std::string index_buffer_var_name = "spvIndices";
	Op previous_instruction_opcode = OpNop;

	// Must be ordered since declaration is in a specific order.
	std::map<uint32_t, MSLConstexprSampler> constexpr_samplers_by_id;
	std::unordered_map<SetBindingPair, MSLConstexprSampler, InternalHasher> constexpr_samplers_by_binding;
	const MSLConstexprSampler *find_constexpr_sampler(uint32_t id) const;

	std::unordered_set<uint32_t> buffers_requiring_array_length;
	SmallVector<uint32_t> buffer_aliases_discrete;
	std::unordered_set<uint32_t> atomic_image_vars_emulated; // Emulate texture2D atomic operations
	std::unordered_set<uint32_t> pull_model_inputs;
	std::unordered_set<uint32_t> recursive_inputs;

	SmallVector<SPIRVariable *> entry_point_bindings;

	// Must be ordered since array is in a specific order.
	struct DynamicBuffer
	{
		uint32_t base_index;
		uint32_t var_id;
		std::string mbr_name;
	};
	std::map<SetBindingPair, DynamicBuffer> buffers_requiring_dynamic_offset;

	SmallVector<uint32_t> disabled_frag_outputs;

	std::unordered_set<SetBindingPair, InternalHasher> inline_uniform_blocks;

	uint32_t argument_buffer_ids[kMaxArgumentBuffers];
	uint32_t argument_buffer_discrete_mask = 0;
	uint32_t argument_buffer_device_storage_mask = 0;

	void emit_argument_buffer_aliased_descriptor(const SPIRVariable &aliased_var,
	                                             const SPIRVariable &base_var);

	void analyze_argument_buffers();

	// AppGL fork (msl-tcs-output-classification.patch): for TCS shaders
	// under `split_tcs_outputs_by_consumption`, classifies each user-varying
	// output as TES-consumed (kept in main0_out / per-CP device buffer)
	// or TCS-internal (routed to threadgroup memory by the existing
	// remapped-storage mechanism). Run after `update_active_builtins`
	// in `compile_internal`, before any emission walks the variables.
	void classify_tcs_outputs_by_consumption();

	bool descriptor_set_is_argument_buffer(uint32_t desc_set) const;
	const MSLResourceBinding &get_argument_buffer_resource(uint32_t desc_set, uint32_t arg_idx) const;
	void add_argument_buffer_padding_buffer_type(SPIRType &struct_type, uint32_t &mbr_idx, uint32_t &arg_buff_index, MSLResourceBinding &rez_bind);
	void add_argument_buffer_padding_image_type(SPIRType &struct_type, uint32_t &mbr_idx, uint32_t &arg_buff_index, MSLResourceBinding &rez_bind);
	void add_argument_buffer_padding_sampler_type(SPIRType &struct_type, uint32_t &mbr_idx, uint32_t &arg_buff_index, MSLResourceBinding &rez_bind);
	void add_argument_buffer_padding_type(uint32_t mbr_type_id, SPIRType &struct_type, uint32_t &mbr_idx, uint32_t &arg_buff_index, uint32_t count);

	uint32_t get_target_components_for_fragment_location(uint32_t location) const;
	uint32_t build_extended_vector_type(uint32_t type_id, uint32_t components,
	                                    SPIRType::BaseType basetype = SPIRType::Unknown);
	uint32_t build_msl_interpolant_type(uint32_t type_id, bool is_noperspective);

	bool suppress_missing_prototypes = false;
	bool suppress_incompatible_pointer_types_discard_qualifiers = false;
	bool suppress_sometimes_unitialized = false;

	void add_spv_func_and_recompile(SPVFuncImpl spv_func);

	void activate_argument_buffer_resources();

	bool type_is_msl_framebuffer_fetch(const SPIRType &type) const;
	bool is_supported_argument_buffer_type(const SPIRType &type) const;

	bool variable_storage_requires_stage_io(StorageClass storage) const;

	bool needs_manual_helper_invocation_updates() const
	{
		return msl_options.manual_helper_invocation_updates && msl_options.supports_msl_version(2, 3);
	}
	bool needs_frag_discard_checks() const
	{
		return get_execution_model() == ExecutionModelFragment && msl_options.supports_msl_version(2, 3) &&
		       msl_options.check_discarded_frag_stores && frag_shader_needs_discard_checks;
	}

	bool has_additional_fixed_sample_mask() const { return msl_options.additional_fixed_sample_mask != 0xffffffff; }
	std::string additional_fixed_sample_mask_str() const;

	// OpcodeHandler that handles several MSL preprocessing operations.
	struct OpCodePreprocessor : OpcodeHandler
	{
		explicit OpCodePreprocessor(CompilerMSL &compiler_)
		    : OpcodeHandler(compiler_), self(compiler_)
		{
			enable_result_types = true;
		}

		bool handle(Op opcode, const uint32_t *args, uint32_t length) override;
		CompilerMSL::SPVFuncImpl get_spv_func_impl(Op opcode, const uint32_t *args, uint32_t length);
		void check_resource_write(uint32_t var_id);

		CompilerMSL &self;
		std::unordered_map<uint32_t, uint32_t> image_pointers_emulated; // Emulate texture2D atomic operations
		bool suppress_missing_prototypes = false;
		bool uses_atomics = false;
		bool uses_image_write = false;
		bool uses_buffer_write = false;
		bool uses_discard = false;
		bool needs_local_invocation_index = false;
		bool needs_subgroup_invocation_id = false;
		bool needs_subgroup_size = false;
		bool needs_sample_id = false;
		bool needs_helper_invocation = false;
		bool uses_cooperative_matrix = false;
	};

	// OpcodeHandler that scans for uses of sampled images
	struct SampledImageScanner : OpcodeHandler
	{
		explicit SampledImageScanner(CompilerMSL &compiler_)
		    : OpcodeHandler(compiler_), self(compiler_)
		{
		}

		CompilerMSL &self;
		bool handle(Op opcode, const uint32_t *args, uint32_t) override;
	};

	// Sorts the members of a SPIRType and associated Meta info based on a settable sorting
	// aspect, which defines which aspect of the struct members will be used to sort them.
	// Regardless of the sorting aspect, built-in members always appear at the end of the struct.
	struct MemberSorter
	{
		enum SortAspect
		{
			LocationThenBuiltInType,
			Offset,
			// Path J' Option E.3 (Sprint 6 Phase 1 — Class 2C range-gated):
			// builtins go to the end (matching LocationThenBuiltInType);
			// non-builtins are sorted by their position in `insertion_order`
			// when matched, fall through to location-then-component
			// otherwise. Used for input interface blocks when the
			// orchestrator registered synthetic-range names via
			// `add_msl_shader_input` to drive cross-stage emission layout.
			InsertionOrderThenLocationThenBuiltInType
		};

		void sort();
		bool operator()(uint32_t mbr_idx1, uint32_t mbr_idx2);
		MemberSorter(SPIRType &t, Meta &m, SortAspect sa);

		SPIRType &type;
		Meta &meta;
		SortAspect sort_aspect;
		// Path J' Option E.3: pointer to the parent compiler's
		// `inputs_by_location_insertion_order` list. Owned by CompilerMSL;
		// MemberSorter only reads. Null when no orchestrator-driven
		// reorder is active.
		const std::vector<std::string> *insertion_order = nullptr;
	};
};
} // namespace SPIRV_CROSS_NAMESPACE

#endif
