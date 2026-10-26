// SPIR-V reading for the Vulkan replay (nvn_vk.c): the translator's shaders (dmsh_xlate, the Ryujinx translator) declare every resource in set 0 with its own binding numbers per stage,
// named as the build's reflection json names them (fp_c8, vp_t_tcb_10, …). This finds each resource's name, binding and descriptor kind, and moves a stage's bindings by an offset so the
// stages of one program can share one push-descriptor set.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SPV_RES_UBO = 1, SPV_RES_SSBO, SPV_RES_TEX, SPV_RES_TBUF, SPV_RES_IMG, SPV_RES_IBUF, SPV_RES_SAMP, SPV_RES_SIMG };   // SIMG: separate sampled image; TEX: combined image + sampler
enum { SPV_DIM_1D = 0, SPV_DIM_2D, SPV_DIM_3D, SPV_DIM_CUBE, SPV_DIM_RECT, SPV_DIM_BUFFER, SPV_DIM_SUBPASS };
typedef struct {
    char name[48];
    uint32_t set, binding, count;     // count: array length (1 = not an array)
    uint8_t kind;                     // SPV_RES_*
    uint8_t dim, arrayed, ms, depth;  // images: SPV_DIM_*, arrayed, multisampled, depth comparison
    uint8_t sclass;                   // images: sampled component class 0 float / 1 uint / 2 sint
} spv_res;
typedef struct { uint32_t location; uint8_t sclass, ncomp; } spv_input;   // vertex stage inputs: location, 0 float / 1 uint / 2 sint, component count

// 0 on success; *stage = SPIR-V execution model (0 vertex, 1 tess control, 2 tess eval, 3 geometry, 4 fragment, 5 compute); local_size: compute workgroup size (may be NULL)
int spv_reflect(const uint32_t *code, size_t words, spv_res *res, int max_res, int *nres, spv_input *in, int max_in, int *nin, int *stage, uint32_t local_size[3]);
// Add `add` to every Binding decoration (in place)
void spv_rebind(uint32_t *code, size_t words, uint32_t add);
// User (Location-decorated, not built-in) Input and Output variables of the entry point as location bit masks (an array takes as many locations as its length)
void spv_io_masks(const uint32_t *code, size_t words, uint64_t *in_mask, uint64_t *out_mask);
// A copy (malloc'd, *out_words long) in which the Input variables at the locations in drop become Private variables holding (0, 0, 0, 1): for a fragment shader reading
// varyings the previous stage never writes (undefined values on the Switch, a pipeline error on Vulkan). NULL if there is nothing to drop or the code cannot be read
uint32_t *spv_drop_inputs(const uint32_t *code, size_t words, uint64_t drop, size_t *out_words);
// A copy in which every user vector output of the entry point starts at the value an unwritten Maxwell attribute reads, (0, 0, 0, 1): vertex / tessellation evaluation
// shaders that write only some components (or none) of an output the next stage reads (dmsh_build.py vs_init / vs_pad on the Metal side). NULL if nothing to do
uint32_t *spv_init_outputs(const uint32_t *code, size_t words, size_t *out_words);
// Drops capabilities / extensions / execution modes a shader declares but never uses (Float64, fragment shader interlock), in place; returns the new length in words
size_t spv_strip_unused(uint32_t *code, size_t words);
// Fragment outputs by location: 0 float, 1 uint, 2 sint, 255 none
void spv_frag_outputs(const uint32_t *code, size_t words, uint8_t cls[8]);
// A copy whose float outputs at the locations in umask / smask hold the same bits as uint / int outputs (integer colour targets). NULL if nothing to do or not possible
uint32_t *spv_int_outputs(const uint32_t *code, size_t words, uint32_t umask, uint32_t smask, size_t *out_words);
// A copy of a vertex shader that stores its transform feedback captures into storage buffers (binding0 + b in set 0): tf[b] = { output location, components } (components 0 = buffer unused),
// at word (VertexIndex - first) * components + base[b], first and base[4] in push constants (5 uints). NULL if not possible
uint32_t *spv_tf_variant(const uint32_t *code, size_t words, const int tf[4][2], uint32_t binding0, size_t *out_words);
// A copy that declares StorageImageWriteWithoutFormat / StorageImageReadWithoutFormat where the shader writes / reads a storage image of format Unknown without them. NULL if none is missing
uint32_t *spv_image_caps(const uint32_t *code, size_t words, size_t *out_words);
