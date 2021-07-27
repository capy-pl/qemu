#include "vmi/vmi.h"
#include "qemu/ctype.h"
#include "qemu/osdep.h"
#include "cpu.h"
#include "string.h"

// init_task address
static const vaddr init_task_address = 0xffff800011b82e40;

// task_struct member offset
static const vaddr tasks_offset = 0x420;
static const vaddr comm_offset = 0x6d8;
// static const vaddr pid_offset = 0x524;
static const vaddr mm_offset = 0x470;
static const vaddr pgd_offset = 0x40;

// kernel linear mapping area offset. by substract the number
// from the kernel virtual address, we can get the physical address
static const vaddr lm_offset = 0xfffeffffc0000000;

// base address mask for ttbr0
static const vaddr badr_mask = 0xffffffffffff;

// vmi internal state
// 0 for uninitialized, 1 for listening.
static int vmi_state = 0;
static uint64_t prev_ttbr0;

// target ps state
static char *target_ps;
static vaddr target_pgd;
static FILE *log_file;

static bool vmi_read_vaddr(CPUState *cs, vaddr addr, uint8_t *buf, int length) {
    // Not a good idea in tcg
    // while (cpu_memory_rw_debug(cs, addr, buf, length, 0) != 0){
    //     printf("Error: cannot read.\n");
    // }
    if (cpu_memory_rw_debug(cs, addr, buf, length, 0) == 0) {
        return 1;
    }
    return 0;
}

static vaddr buffer_to_pointer (uint8_t *buf) {
    vaddr p = 0;
    int i;
    for (i = 7; i >=0; i--) {
        p = p << 8 | buf[i];
    }
    return p;
}

// clear previous target ps state
static void clear_state() {
    if (target_ps) {
        free(target_ps);
    }

    if (target_pgd) {
        target_pgd = NULL;
    }

    if (log_file) {
        fclose(log_file);
    }
}

bool vmi_is_enabled(void) {
    return vmi_state;
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

// TODO: find a way to detect infinite loop and end it.
// if success 1, else 0
bool vmi_get_ps_pgd(CPUState *cs, const char *ps_name, vaddr *target_pgd) {
    vaddr task_start_address = init_task_address;
    vaddr next_pointer = 0x0;
    vaddr prev_task_address = 0x0;
    vaddr mm_pointer = 0x0;
    uint8_t pointer_buf[8];
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
            // read mm pointer
            if (!vmi_read_vaddr(cs, task_start_address + mm_offset, pointer_buf, 8)) {
                return 0;
            }
    
            mm_pointer = buffer_to_pointer(pointer_buf);

            if (!vmi_read_vaddr(cs, mm_pointer + pgd_offset, pointer_buf, 8)) {
                return 0;
            }
            *target_pgd = buffer_to_pointer(pointer_buf) - lm_offset;
            return 1;
        }

        if (!vmi_read_vaddr(cs, task_start_address + tasks_offset, pointer_buf, 8)){
            return 0;
        }
    
        prev_task_address = task_start_address;
        next_pointer = buffer_to_pointer(pointer_buf);
        task_start_address = next_pointer - tasks_offset;
    }
    return 0;
}

void vmi_init() {
    vmi_state = 1;
}

void vmi_introspect(CPUState *cs) {
    fprintf(log_file, "Process %s is detected.\n", target_ps);
    fprintf(log_file, "\t\t\t\tpgd = %llx\n", target_pgd);

    vmi_stop();
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
