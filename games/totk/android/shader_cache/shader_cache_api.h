/* Internal module API. */
#ifndef TOTK_ANDROID_SHADER_CACHE_API_0_H
#define TOTK_ANDROID_SHADER_CACHE_API_0_H
#include "shader_cache_state.h"

const char *runtime_shader_stage_name(uint32_t stage);

int is_spv(const uint8_t *p, size_t n);

int control_declared_length(const void *control_v, uintptr_t region_start, size_t region_size, uint32_t *length_out, uint32_t *stage_out);

int sph_ok(const uint8_t *region, size_t region_size, size_t off, uint32_t length, uint32_t stage);

int find_bahs(const uint8_t *region, size_t region_size, size_t control_off, size_t *header_out, uint32_t *pool_out, uint32_t *pool_size_out);

int resolve_in_region_impl(const void *region_v, size_t region_size, const void *control, uint64_t low_addr, const void **code, RuntimeShaderResolveInfo *info, int pointer_ok);

int runtime_shader_resolve_in_region(const void *region_v, size_t region_size, const void *control, uint64_t low_addr, const void **code, RuntimeShaderResolveInfo *info);

int runtime_shader_resolve_code(const void *control, uint64_t low_addr, const void **code, RuntimeShaderResolveInfo *info);

int runtime_shader_identity(const void *control_v, const void *code_v, RuntimeShaderIdentity *out);

const char *runtime_shader_cache_root(void);

int mkdir_p(const char *path);

int read_file(const char *path, uint8_t **out, size_t *size);

int ref_path_for(const char *spv_path, char *out, size_t cap);

int file_exists(const char *path);

int try_candidate(const char *root, const char *subdir, const char *name, RuntimeShaderModule *out);

int find_variant(const char *root, const char *subdir, const char *key, char *name, size_t cap);

int runtime_shader_cache_load(const char *root, const RuntimeShaderIdentity *id, RuntimeShaderModule *out);

int suffix_ok(const char *s);

int runtime_shader_cache_load_variant(const char *root, const RuntimeShaderIdentity *id, const char *suffix, RuntimeShaderModule *out);

int runtime_shader_cache_has_variants(const char *root, const RuntimeShaderIdentity *id);

void runtime_shader_cache_module_free(RuntimeShaderModule *module);

int write_file(const char *path, const void *data, size_t size);

int runtime_shader_cache_store(const char *root, const RuntimeShaderIdentity *id, const uint8_t *spv, size_t spv_size, const char *res_json);

#endif
