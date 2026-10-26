// Darwin stand-in (dm/android/compat): the Mach calls the host makes, on Linux / bionic
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <mach/mach_time.h>
typedef int kern_return_t;
typedef unsigned int mach_port_t, thread_act_t, task_t, mach_msg_type_number_t, natural_t;
typedef uintptr_t vm_address_t, vm_offset_t, mach_vm_address_t;
typedef size_t vm_size_t, mach_vm_size_t;
typedef int vm_prot_t, boolean_t, thread_flavor_t, task_flavor_t, vm_region_flavor_t;
typedef natural_t *thread_state_t, *task_info_t, *thread_info_t, *vm_region_info_t;
typedef thread_act_t *thread_act_array_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define KERN_INVALID_ADDRESS 1
#define TRUE 1
#define FALSE 0
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
#define MACH_PORT_NULL 0
// Thread state (crash / sampling dumps): Mach thread suspension has no Android equivalent; the calls fail and the dumps skip those threads
typedef struct { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(natural_t)))
static inline mach_port_t mach_task_self(void) { return 1; }
mach_port_t mach_thread_self(void);
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }
static inline kern_return_t thread_suspend(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_resume(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_get_state(thread_act_t t, int f, thread_state_t s, mach_msg_type_number_t *n) { (void)t; (void)f; (void)s; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_info(thread_act_t t, int f, thread_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
static inline kern_return_t task_threads(task_t t, thread_act_array_t *a, mach_msg_type_number_t *n) { (void)t; *a = 0; *n = 0; return KERN_FAILURE; }
static inline kern_return_t task_info(task_t t, int f, task_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
kern_return_t vm_read_overwrite(task_t t, vm_address_t a, vm_size_t n, vm_address_t to, vm_size_t *got);   // process_vm_readv on this process: a safe read of possibly unmapped memory
kern_return_t vm_deallocate(task_t t, vm_address_t a, vm_size_t n);
kern_return_t vm_allocate(task_t t, vm_address_t *a, vm_size_t n, int flags);
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
#define THREAD_BASIC_INFO 3
#define TASK_VM_INFO 22
#define TASK_POWER_INFO_V2 26
#define TASK_THREAD_TIMES_INFO 3
// VM region queries (diagnostic only): no equivalent wired up, the caller reports "(none)"
typedef struct { int protection, max_protection; } vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
static inline kern_return_t vm_region_64(task_t t, vm_address_t *a, vm_size_t *s, int f, vm_region_info_t i, mach_msg_type_number_t *n, mach_port_t *o) { (void)t; (void)a; (void)s; (void)f; (void)i; (void)n; (void)o; return KERN_FAILURE; }
// Thread run state (sampler)
typedef struct { int run_state, flags, suspend_count, sleep_time; struct { int seconds, microseconds; } user_time, system_time; int cpu_usage, policy; } thread_basic_info_data_t;
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(natural_t)))
#define TH_STATE_RUNNING 1
#define TH_STATE_WAITING 3
// Task / thread accounting (diagnostic HUD lines): the queries fail, the callers print nothing
typedef struct { uint64_t virtual_size, resident_size, phys_footprint; } task_vm_info_data_t;
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(natural_t)))
typedef struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups; } dm_task_cpu_energy;
typedef struct { uint64_t task_gpu_utilisation; } dm_task_gpu_energy;
typedef struct { dm_task_cpu_energy cpu_energy; dm_task_gpu_energy gpu_energy; uint64_t task_energy, task_ptime; } task_power_info_v2_data_t;
#define TASK_POWER_INFO_V2_COUNT ((mach_msg_type_number_t)(sizeof(task_power_info_v2_data_t) / sizeof(natural_t)))
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
typedef natural_t *thread_policy_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(natural_t)))
static inline kern_return_t thread_policy_set(thread_act_t t, int f, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)f; (void)p; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_get(thread_act_t t, int f, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *d) { (void)t; (void)f; (void)p; (void)n; (void)d; return KERN_FAILURE; }
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(natural_t)))
// vm_remap (Monster Hunter Rise's sparse GPU pool): not wired up on Android yet
static inline kern_return_t vm_remap(task_t t, vm_address_t *to, vm_size_t n, vm_address_t mask, int flags, task_t src, vm_address_t from, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, int inh) {
    (void)t; (void)to; (void)n; (void)mask; (void)flags; (void)src; (void)from; (void)copy; (void)cur; (void)max; (void)inh; return KERN_FAILURE;
}
