#include "qemu/ctype.h"
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"

void vmi_init(void);
bool vmi_listen(CPUState *cs, const char *target_ps_name, const char *file_path);
bool vmi_has_context_switch(ARMCPU *armcpu);
void vmi_check_pgd(CPUState *cs);
void vmi_stop(void);
bool vmi_is_target_ps(CPUState *cs);
bool vmi_get_ps_pgd(CPUState *cs, const char *ps_name, vaddr *target_pgd);

void vmi_enter_introspect(CPUState *cs, TranslationBlock *tb);
void vmi_exit_introspect(void);

bool vmi_is_enabled(void);
bool vmi_is_entered(void);
