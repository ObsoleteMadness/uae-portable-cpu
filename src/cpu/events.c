/*
 * events.c
 *
 * Event stuff - currently just simplified to the bare minimum
 * in Hatari, but we might want to extend this one day...
 */


#include "sysconfig.h"
#include "sysdeps.h"

#include "options_cpu.h"
#include "events.h"
#include "host_hooks.h"

#ifdef JIT
int pissoff_value = 0;
int pissoff_nojit_value = 0;
int pissoff = 0;
#endif

#ifndef WINUAE_FOR_HATARI
void do_cycles_normal(int cycles_to_add)
{
	while ((nextevent - currcycle) <= cycles_to_add) {

		cycles_to_add -= (int)(nextevent - currcycle);
		currcycle = nextevent;

		for (int i = 0; i < ev_max; i++) {
			if (eventtab[i].active && eventtab[i].evtime == currcycle) {
				if (eventtab[i].handler == NULL) {
					gui_message(_T("eventtab[%d].handler is null!\n"), i);
					eventtab[i].active = 0;
				} else {
					(*eventtab[i].handler)();
				}
			}
		}
		events_schedule();

	}
	currcycle += cycles_to_add;
}
#else
/* Simplified version for Hatari, we don't use eventtab[] */
void do_cycles_normal(int cycles_to_add)
{
//fprintf ( stderr , "  do_cycles_normal add=%d curr=%d -> new=%d\n" , cycles_to_add , currcycle , currcycle+cycles_to_add );
	currcycle += cycles_to_add;
}
#endif


void do_cycles_slow (int cycles_to_add)
{
	/* Compiled code keeps its own countdown; fold it in while the JIT runs. */
	if (g_jit_run_active) {
		uae_host_jit_do_cycles(cycles_to_add);
		return;
	}
//fprintf ( stderr , "  do_cycles_slow add=%d curr=%d -> new=%d\n" , cycles_to_add , currcycle , currcycle+cycles_to_add );
	currcycle += cycles_to_add;
}
