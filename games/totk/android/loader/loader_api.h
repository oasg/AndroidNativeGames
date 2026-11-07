/* Internal module API. */
#ifndef TOTK_ANDROID_LOADER_API_0_H
#define TOTK_ANDROID_LOADER_API_0_H
#include "loader_state.h"

void runtime_guest_commit(uintptr_t base, uintptr_t size);

void runtime_guest_stack_info(uint64_t *base, uint64_t *size);

void runtime_guest_commit(uintptr_t base, uintptr_t size);

uint32_t runtime_thread_current_handle(void);

uint64_t runtime_guest_call(void *fn, uintptr_t stack_top,
                            uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4);

void *runtime_override(const char *name);

void *runtime_observer(const char *name);

void *runtime_hle_stub(const char *name);

uint64_t runtime_svc(unsigned num, uint64_t *r, const char *module, uint64_t offset);

const char *runtime_nvn_last_api(void);

const char *runtime_nvn_last_proc(void);

void runtime_thread_prepare_stack(uintptr_t top);

void ipc_key_init(void);

void ctx_key_init(void);

void runtime_trace_guest(uint64_t fp, uint64_t lr);

_Noreturn void runtime_fail(const char *message);

void need(bool c, const char *m);

size_t page_size(void);

size_t page_up(size_t x);

uint64_t now_ns(void);

uint64_t runtime_now_ns(void);

uintptr_t read_tpidr_el0(void);

void runtime_thread_set_guest_handle(uint32_t handle);

void runtime_thread_init(void);

uintptr_t runtime_guest_stack_top(void);

void runtime_hle_stack_set(uintptr_t top);

void runtime_hle_stack_init(uint64_t off);

void *runtime_symbol(const char *name);

uint8_t *runtime_module_base(const char *name);

uint64_t runtime_image_bytes(void);

void runtime_dispatch(uint64_t ret, uint64_t *regs);

void crash_altstack_install(void);

int runtime_guest_code_pc(uint64_t pc);

int runtime_guest_top_level(uint64_t pc);

void runtime_describe_address(uint64_t addr, char *out, size_t cap);

void describe_address(uint64_t addr, char *out, size_t cap);

int crash_read(uint64_t addr, void *out, size_t n);

void runtime_guest_frame_report(uint64_t fp, uint64_t lr);

void chain_append(char *out, size_t cap, size_t *n, const char *s);

int runtime_guest_chain(uint64_t lr, uint64_t fp, char *out, size_t cap);

void crash_handler(int sig, siginfo_t *si, void *ctx);

unsigned hash_name(const char *s);

void make_hash(Module *m);

void *module_symbol(Module *m, const char *name);

void map_module(Module *m, const char *dir);

void relocate(Module *m);

void seal(Module *m);

int inits_enabled(void);

void *fs_entry(const char *name);

void fs_probe(void);

uint64_t run_init_array(Module *m);

void module_inits(void);

void module_finis(void);

char *read_text_file(const char *path, size_t *size);

void *probe_thread(void *unused);

void loader_ready(void);

int probe_vulkan_triangle(const char *dir);

int boot_sdk(void);

int probe_hle(void);

int probe(void);

int loader_main(int argc, char **argv);

uint64_t host_add(uint64_t a, uint64_t b);

uint64_t guest_call1(void *fn, uint64_t a0);

uint64_t guest_call2(void *fn, uint64_t a0, uint64_t a1);

void *tls_probe_thread(void *p);

void tls_probe(void);

unsigned reflect_cbufs(const char *path, uint32_t *out, unsigned max);

const char *json_member(const char *obj, const char *end, const char *key);

long json_long(const char *obj, const char *end, const char *key, long dflt);

int json_bool(const char *obj, const char *end, const char *key, int dflt);

void json_string(const char *obj, const char *end, const char *key, char *out, size_t out_size);

uint32_t reflect_texture_format(const char *name);

GfxBindingKind reflect_json_kind(const char *section, long type, int separate);

unsigned reflect_json_section(const char *text, const char *section, GfxBindingSpec *out, unsigned max);

unsigned reflect_json_file(const char *path, GfxBindingSpec *out, unsigned max);

unsigned reflect_merge(GfxBindingSpec *out, unsigned max, const GfxBindingSpec *json, unsigned njson, const GfxBindingSpec *spv, unsigned nspv);

unsigned gfx_reflect_full(const char *json_path, const char *spv_path, uint32_t target_set, GfxBindingSpec *out, unsigned max);

const char *reflect_kind_name(GfxBindingKind k);

int reflect_has_non_ubo(const char *path);

void reflect_log(const char *stage, const GfxBindingSpec *s, unsigned n);

int reflect_spirv_file(const char *path, uint32_t target_set, GfxBindingSpec *out, unsigned max);

int spirv_interface_locations(const char *path, int fragment, uint32_t *out, unsigned max);

int reflect_check_interface(const char *vs_path, const char *ps_path);

int gfx_spirv_move_set(uint8_t *code, size_t size, uint32_t from, uint32_t to);

int totk_android_loader_entry(void *host_window, int argc, char **argv);

int main(int argc, char **argv);

#endif
