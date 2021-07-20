#include "vmi/vmi.h"
#include "qemu/ctype.h"
#include "qemu/osdep.h"
#include "cpu.h"

int hello_world(void) {
    return 0;
}

vaddr read_pointer_vir(CPUState *cs, vaddr addr) {
    uint8_t buf[8];
    int i = 0;
    vaddr val = 0;
    cpu_memory_rw_debug(cs, addr, buf, 8, 0);
    for (i = 7;i >= 0;i--) {
        val = val << 8 | buf[i];
    }
    return val;
}
