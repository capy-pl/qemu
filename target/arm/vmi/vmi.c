#include "vmi/vmi.h"
#include "qemu/ctype.h"
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "string.h"
#include "exec/exec-all.h"
#include "monitor/monitor-internal.h"

#define QEMU_HOST_CLOCK_TIME qemu_clock_get_us(QEMU_CLOCK_HOST) 

// init_task address
static const vaddr init_task_address = 0xffff800012052ec0;

// task_struct member offset
static const vaddr tasks_offset = 0x420;
static const vaddr comm_offset = 0x6d8;
static const vaddr mm_offset = 0x470;
static const vaddr pgd_offset = 0x40;
static const vaddr pid_offset = 0x524;

// user library function start address
// malloc, free, fopen, fclose
static const uint64_t memcpy_address = 0xfffff7db0750;
static const uint64_t memmove_address = 0xfffff7db0090;
static const uint64_t strlen_address = 0xfffff7ea9cd8;
static const uint64_t strncpy_address = 0xfffff7eaa0d8;
static const uint64_t getenv_address = 0xfffff7e61e90;
static const uint64_t thrd_join_address = 0xfffff7fb1860;
static const uint64_t thrd_create_address = 0xfffff7fb17a0;
static const uint64_t thrd_exit_address = 0xfffff7fb1850;
static const uint64_t printf_address = 0xfffff7d7b2f8;
static const uint64_t scanf_address = 0xfffff7d7b7a0;
static const uint64_t malloc_address = 0xfffff7fe22f0;
static const uint64_t calloc_address = 0xfffff7fe2418;
static const uint64_t realloc_address = 0xfffff7fe2618;
static const uint64_t free_address = 0xfffff7fe2450;
static const uint64_t fopen_address = 0xfffff7d95140;
static const uint64_t fclose_address = 0xfffff7d94658;
static const uint64_t fwrite_address = 0xfffff7d95b50;
static const uint64_t fread_address = 0xfffff7d955c8;

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

// stack state
static uint64_t return_address;

static bool vmi_read_vaddr(CPUState *cs, vaddr addr, uint8_t *buf, int length) {
    return cpu_memory_rw_debug(cs, addr, buf, length, 0) == 0;
}

static uint64_t buffer_to (uint8_t *buf, uint size) {
    uint64_t p = 0;
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

static void print_string_literal(char *str, int len) {
    for (int i = 0;i < len;i++) {
        switch (str[i])
        {
        case '\n':
            fprintf(log_file, "\\n");
            break;

        default:
            fprintf(log_file, "%c", str[i]);
            break;
        }
    }
}

bool vmi_is_enabled(void) {
    return vmi_state;
}

bool vmi_is_entered(void) {
    return vmi_entered;
}

void vmi_check_pgd(ARMCPU *armcpu) {
    if (target_pgd) return;
    if (vmi_has_context_switch(armcpu)) {
        prev_ttbr0 = armcpu->env.cp15.ttbr0_ns;
        vmi_get_ps_pgd(armcpu, target_ps, &target_pgd);
    }
}

bool vmi_has_context_switch(ARMCPU *armcpu) {
    return armcpu->env.cp15.ttbr0_ns != prev_ttbr0;
}

bool vmi_is_target_ps(ARMCPU *armcpu) {
    return target_pgd && (armcpu->env.cp15.ttbr0_ns & badr_mask) == target_pgd;
}

// if success 1, else 0
bool vmi_get_ps_pgd(ARMCPU *cs, const char *ps_name, vaddr *target_pgd) {
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
            fprintf(log_file, "[%lld] %s detected,pid=%d,ttbr0_el1=%llx\n", QEMU_HOST_CLOCK_TIME, ps_name, pid, *target_pgd);
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

void vmi_enter_introspect(ARMCPU *armcpu, CPUState *cs, TranslationBlock *tb) {
    // TODO: record whether vmi is currently entered or exited
    // vmi_entered = 1;

     if (tb->pc == return_address) {
        fprintf(log_file, "[%lld] pc=0x%llx, return value=%lld\n", QEMU_HOST_CLOCK_TIME, armcpu->env.pc, armcpu->env.xregs[0]);
        return_address = NULL;
     }

    // test change return value in registers
    // if (tb->pc == return_address) {
    //     armcpu->env.xregs[0] = 10;
    // }

    if (tb->pc == printf_address) {
        uint8_t arg1_buffer[256] = {"\0"};
        vmi_read_vaddr(cs, armcpu->env.xregs[0], arg1_buffer, 128);
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function=printf, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1=");
        print_string_literal((char *)arg1_buffer, strlen((char *)arg1_buffer));
        fprintf(log_file, "\n");
        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == malloc_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function=malloc, ");
        fprintf(log_file, "args1(size)=%llu\n", armcpu->env.xregs[0]);
        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == calloc_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function=calloc, ");
        fprintf(log_file, "args1(num)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(size)=%llu\n", armcpu->env.xregs[1]);
    }

    if (tb->pc == realloc_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function=realloc, ");
        fprintf(log_file, "args1(ptr)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(size)=%llu\n", armcpu->env.xregs[1]);
    }

    if (tb->pc == free_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function=free, ");
        fprintf(log_file, "args1(ptr)=%llu\n", armcpu->env.xregs[0]);
    }

    if (tb->pc == memcpy_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= memcpy, ");
        fprintf(log_file, "args1(destination)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(source)=%llu\n", armcpu->env.xregs[1]);
        fprintf(log_file, "args3(num)=%llu\n", armcpu->env.xregs[2]);
    }

    if (tb->pc == memmove_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= memmove, ");
        fprintf(log_file, "args1(destination)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(source)=%llu\n", armcpu->env.xregs[1]);
        fprintf(log_file, "args3(count)=%llu\n", armcpu->env.xregs[2]);
    }

    if (tb->pc == strncpy_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= strncpy, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1(destination)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(source)=%llu\n", armcpu->env.xregs[1]);
        fprintf(log_file, "args3(count)=%llu\n", armcpu->env.xregs[2]);
        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == fopen_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= strncpy, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1(filename)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(mode)=%llu\n", armcpu->env.xregs[1]);
        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == fclose_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= fclose, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1(stream)=%llu\n", armcpu->env.xregs[0]);
        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == thrd_create_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= thrd_create, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1(thr)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(func)=%llu\n", armcpu->env.xregs[1]);
        fprintf(log_file, "args2(arg)=%llu\n", armcpu->env.xregs[2]);

        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == thrd_exit_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= thrd_exit, ");
        fprintf(log_file, "args1(res)=%llu\n", armcpu->env.xregs[0]);
    }

    if (tb->pc == thrd_join_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= thrd_join, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1(thr)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(res)=%llu\n", armcpu->env.xregs[1]);
        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == getenv_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= getenv, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1(name)=%llu\n", armcpu->env.xregs[0]);
        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == fread_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= fread, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1(ptr)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args2(size)=%llu\n", armcpu->env.xregs[1]);
        fprintf(log_file, "args3(count)=%llu\n", armcpu->env.xregs[2]);
        return_address = armcpu->env.xregs[30];
    }

    if (tb->pc == fopen_address) {
        fprintf(log_file, "[%lld] cpu=%d, pc=0x%llx, ", QEMU_HOST_CLOCK_TIME, cs->cpu_index, armcpu->env.pc);
        fprintf(log_file, "function= fopen, ");
        fprintf(log_file, "return address=0x%llx, ", armcpu->env.xregs[30]);
        fprintf(log_file, "args1(filename)=%llu\n", armcpu->env.xregs[0]);
        fprintf(log_file, "args1(mode)=%llu\n", armcpu->env.xregs[1]);
        return_address = armcpu->env.xregs[30];
    }
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
    fflush(log_file);
    vmi_state = 0;
    clear_state();
}
