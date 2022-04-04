#include "qemu/ctype.h"
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "monitor/monitor-internal.h"

void vmi_init(void);
bool vmi_listen(CPUState *cs, const char *target_ps_name, const char *file_path);
bool vmi_has_context_switch(ARMCPU *armcpu);
void vmi_check_pgd(ARMCPU *armcpu);
void vmi_stop(void);
bool vmi_is_target_ps(ARMCPU *armcpu);
bool vmi_get_ps_pgd(ARMCPU *cs, const char *ps_name, vaddr *target_pgd);
void vmi_enter_introspect(ARMCPU *armcpu, CPUState *cs, TranslationBlock *tb);
void vmi_exit_introspect(void);
bool vmi_is_enabled(void);
bool vmi_is_entered(void);
