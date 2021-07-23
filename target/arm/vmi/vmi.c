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

// 0 for uninitialized, 1 for listening.
static int vmi_state = 0;

// target ps state
static char *target_ps;
static FILE *log_file;
static vaddr pgd;

static void vmi_read_vaddr(CPUState *cs, vaddr addr, uint8_t *buf, int length) {
    while (cpu_memory_rw_debug(cs, addr, buf, length, 0) != 0){}
}

static vaddr buffer_to_pointer (uint8_t *buf) {
    vaddr p = 0;
    int i;
    for (i = 7; i >=0; i--) {
        p = p << 8 | buf[i];
    }
    return p;
}

// if success 1, else 0
bool vmi_get_ps_pgd(CPUState *cs, const char *ps_name, vaddr *target_pgd) {
    vaddr task_start_address = init_task_address;
    vaddr next_pointer = 0x0;
    vaddr mm_pointer = 0x0;
    uint8_t pointer_buf[8];
    uint8_t comm_buf[16];

    if (ps_name == NULL || strlen(ps_name) == 0) return false;

    while (next_pointer != init_task_address + tasks_offset) {
        // read task comm
        vmi_read_vaddr(cs, task_start_address + comm_offset, comm_buf, 16);

        if (strcmp((char *)comm_buf, ps_name) == 0) {
            // read mm pointer
            vmi_read_vaddr(cs, task_start_address + mm_offset, pointer_buf, 8);
            mm_pointer = buffer_to_pointer(pointer_buf);
            
            vmi_read_vaddr(cs, mm_pointer + pgd_offset, pointer_buf, 8);
            *target_pgd = buffer_to_pointer(pointer_buf) - lm_offset;
            return 1;
        }

        vmi_read_vaddr(cs, task_start_address + tasks_offset, pointer_buf, 8);
        next_pointer = buffer_to_pointer(pointer_buf);
        task_start_address = next_pointer - tasks_offset;
    }
    return 0;
}

void vmi_init() {
    vmi_state = 1;
}

bool vmi_is_current_ps(vaddr ttbr0_el1) {
    ttbr0_el1 = ttbr0_el1 & badr_mask;
    if (vmi_state && ttbr0_el1 == pgd) return true;
    return false;
}

// 0 for failed, 1 for success.
bool vmi_listen(const char *target_ps_name, const char *file_path) {
    if (strlen(target_ps_name) > 16) {
        return 0;
    }

    if (target_ps != NULL) {
        free(target_ps);
    }
    target_ps = malloc(strlen(target_ps_name) * sizeof(char));
    // record target ps name
    strcpy(target_ps, target_ps_name);

    log_file = fopen(file_path, "a");
    return 1;
}

void vmi_stop(void) {

}