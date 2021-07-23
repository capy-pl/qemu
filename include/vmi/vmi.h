#include "qemu/ctype.h"
#include "qemu/osdep.h"
#include "cpu.h"

void vmi_init(void);
bool vmi_listen(const char *target_ps_name, const char *file_path);
void vmi_stop(void);
bool vmi_is_current_ps(vaddr ttbr0_el1);
bool vmi_get_ps_pgd(CPUState *cs, const char *ps_name, vaddr *target_pgd);