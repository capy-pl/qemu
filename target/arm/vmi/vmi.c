#include "vmi/vmi.h"
#include "qemu/ctype.h"
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "string.h"
#include "exec/exec-all.h"
#include "monitor/monitor-internal.h"

#define QEMU_HOST_CLOCK_TIME qemu_clock_get_ms(QEMU_CLOCK_HOST) 

// init_task address
static const vaddr init_task_address = 0xffff800011b82e40;

// task_struct member offset
static const vaddr tasks_offset = 0x420;
static const vaddr comm_offset = 0x6d8;
static const vaddr mm_offset = 0x470;
static const vaddr pgd_offset = 0x40;
static const vaddr pid_offset = 0x524;

// user library function start address
static const target_ulong printf_address = 0xfffff7ea62f8;
static const target_ulong scanf_address = 0xfffff7ea7708;

// kernel linear mapping area offset. by substract the number
// from the kernel virtual address, we can get the physical address
static const vaddr lm_offset = 0xfffeffffc0000000;

// base address mask for ttbr0
static const vaddr badr_mask = 0xffffffffffff;

// vmi internal state
static int vmi_state = 0;   // 0 for uninitialized, 1 for listening
static int vmi_entered = 0; // 0 exit, 1 entered
static uint64_t prev_ttbr0;

// target ps state
static char *target_ps;
static vaddr target_pgd;
static FILE *log_file;
static int pid;

static bool vmi_read_vaddr(CPUState *cs, vaddr addr, uint8_t *buf, int length) {
    return cpu_memory_rw_debug(cs, addr, buf, length, 0) == 0;
}

static vaddr buffer_to (uint8_t *buf, uint size) {
    vaddr p = 0;
    int i;
    for (i = size; i >= 0; i--) {
        p = p << 8 | buf[i];
    }
    return p;
}

// clear previous target ps state
static void clear_state() {
    if (target_ps) {
        free(target_ps);
        target_ps = NULL;
    }

    if (target_pgd) {
        target_pgd = NULL;
    }

    if (pid) {
        pid = NULL;
    }

    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

bool vmi_is_enabled(void) {
    return vmi_state;
}

bool vmi_is_entered(void) {
    return vmi_entered;
}

void vmi_check_pgd(CPUState *cs) {
    ARMCPU *armcpu = ARM_CPU(cs);
    if (target_pgd) return;
    if (vmi_has_context_switch(armcpu)) {
        prev_ttbr0 = armcpu->env.cp15.ttbr0_ns;
        vmi_get_ps_pgd(cs, target_ps, &target_pgd);
    }
}

bool vmi_has_context_switch(ARMCPU *armcpu) {
    return armcpu->env.cp15.ttbr0_ns != prev_ttbr0;
}

bool vmi_is_target_ps(CPUState *cs) {
    ARMCPU *armcpu = ARM_CPU(cs);
    return target_pgd && (armcpu->env.cp15.ttbr0_ns & badr_mask) == target_pgd;
}

// if success 1, else 0
bool vmi_get_ps_pgd(CPUState *cs, const char *ps_name, vaddr *target_pgd) {
    vaddr task_start_address = init_task_address;
    vaddr next_pointer = 0x0;
    vaddr prev_task_address = 0x0;
    vaddr mm_pointer = 0x0;
    uint8_t pointer_buf[8];
    uint8_t pid_buf[4];
    uint8_t comm_buf[16];

    if (ps_name == NULL || strlen(ps_name) == 0) return false;

    while (next_pointer != init_task_address + tasks_offset) {
        // infinite loop detected
        if (prev_task_address == task_start_address) {
            task_start_address = init_task_address;
            next_pointer = 0x0;
            continue;
        }

        // read task comm
        if (!vmi_read_vaddr(cs, task_start_address + comm_offset, comm_buf, 16)) {
            return 0;
        }

        if (strcmp((char *)comm_buf, ps_name) == 0) {
            // read pid
            if (!vmi_read_vaddr(cs, task_start_address + pid_offset, pid_buf, 4)) {
                return 0;
            }

            pid = buffer_to(pid_buf, 4);

            // read mm pointer
            if (!vmi_read_vaddr(cs, task_start_address + mm_offset, pointer_buf, 8)) {
                return 0;
            }
    
            mm_pointer = buffer_to(pointer_buf, 8);

            if (!vmi_read_vaddr(cs, mm_pointer + pgd_offset, pointer_buf, 8)) {
                return 0;
            }

            *target_pgd = buffer_to(pointer_buf, 8) - lm_offset;
            fprintf(log_file, "[%lld] %s detected,pid=%d,ttbr0_el1=%llu\n", QEMU_HOST_CLOCK_TIME, ps_name, pid, *target_pgd);
            fflush(log_file);
            return 1;
        }

        if (!vmi_read_vaddr(cs, task_start_address + tasks_offset, pointer_buf, 8)){
            return 0;
        }
    
        prev_task_address = task_start_address;
        next_pointer = buffer_to(pointer_buf, 8);
        task_start_address = next_pointer - tasks_offset;
    }
    return 0;
}

void vmi_init() {
    vmi_state = 1;
}

void vmi_enter_introspect(CPUState *cs, TranslationBlock *tb) {
    ARMCPU *armcpu = ARM_CPU(cs);

    vmi_entered = 1;

    if (tb->pc == printf_address) {
        fprintf(log_file, "[%lld] ", QEMU_HOST_CLOCK_TIME);
        fprintf(log_file, "pc=%llx,", armcpu->env.pc);
        fprintf(log_file, "Function=printf\n");
    }

    if (tb->pc == scanf_address) {
        fprintf(log_file, "[%lld] ", QEMU_HOST_CLOCK_TIME);
        fprintf(log_file, "pc=%llx,", armcpu->env.pc);
        fprintf(log_file, "Function=scanf\n");
    }

    fflush(log_file);
}

void vmi_exit_introspect(void) {
    vmi_entered = 0;
}

// 0 for failed, 1 for success.
bool vmi_listen(CPUState *cs, const char *target_ps_name, const char *file_path) {
    if (!vmi_state) {
        return 0;
    }

    if (strlen(target_ps_name) > 16) {
        return 0;
    }

    clear_state();

    target_ps = malloc(strlen(target_ps_name) * sizeof(char));
    strcpy(target_ps, target_ps_name);
    log_file = fopen(file_path, "a");

    vmi_get_ps_pgd(cs, target_ps, &target_pgd);
    return 1;
}

void vmi_stop(void) {
    vmi_state = 0;
    clear_state();
}
