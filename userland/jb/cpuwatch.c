/*
 * cpuwatch: every second, the processes using CPU (libproc) and the VM's free/active/compressed pages, one line each to
 * stdout (the job runs it with its output in the kernel log through sweetnull=cpuwatch). To find out what a stall is:
 * a busy process (CPU-bound) or nothing busy (waiting on something). MIT, as userland/.
 */
#include <stdint.h>
/* the iOS SDK hides libproc.h; these are in libSystem */
extern int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
#define PROC_ALL_PIDS    1
#define PROC_PIDTASKINFO 4
struct proc_taskinfo {
	uint64_t pti_virtual_size, pti_resident_size, pti_total_user, pti_total_system, pti_threads_user, pti_threads_system;
	int32_t pti_policy, pti_faults, pti_pageins, pti_cow_faults, pti_messages_sent, pti_messages_received;
	int32_t pti_syscalls_mach, pti_syscalls_unix, pti_csw, pti_threadnum, pti_numrunning, pti_priority;
};
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXP 1024
static pid_t pids[MAXP];
static uint64_t prev[MAXP * 4][2];     /* pid -> total cpu ns, slot by pid hash */

int
main(void)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t last = mach_absolute_time();
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (;;) {
		sleep(1);
		uint64_t now = mach_absolute_time();
		double dt = (double)(now - last) * tb.numer / tb.denom;
		last = now;
		int n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)) / sizeof(pid_t);
		char line[400];
		int off = snprintf(line, sizeof(line), "CPUW:");
		for (int i = 0; i < n; i++) {
			pid_t pid = pids[i];
			struct proc_taskinfo ti;
			if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) != sizeof(ti)) {
				continue;
			}
			uint64_t total = (ti.pti_total_user + ti.pti_total_system) * (uint64_t)tb.numer / tb.denom;
			unsigned slot = (unsigned)pid % (MAXP * 4);
			uint64_t before = prev[slot][1] == (uint64_t)pid ? prev[slot][0] : total;
			prev[slot][0] = total;
			prev[slot][1] = (uint64_t)pid;
			double pct = 100.0 * (double)(total - before) / dt;
			if (pct >= 8.0 && off < (int)sizeof(line) - 40) {
				char name[32] = "?";
				proc_name(pid, name, sizeof(name));
				off += snprintf(line + off, sizeof(line) - off, " %s %.0f%%", name, pct);
			}
		}
		vm_statistics64_data_t vm;
		mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
		if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
			off += snprintf(line + off, sizeof(line) - off, " | free %lluMB active %lluMB wired %lluMB compressed %lluMB",
			    (unsigned long long)vm.free_count * 16 / 1024, (unsigned long long)vm.active_count * 16 / 1024,
			    (unsigned long long)vm.wire_count * 16 / 1024, (unsigned long long)vm.compressor_page_count * 16 / 1024);
		}
		puts(line);
	}
}
