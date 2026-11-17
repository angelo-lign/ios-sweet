/*
 * tsample: a sampling profiler for one process (iOS has no `sample`).
 *   tsample PID|NAME [SAMPLES] [INTERVAL_US] [DELAY_S] [TRIG_PCT]   (a process name is looked up, after DELAY_S seconds; TRIG_PCT: wait for a thread that busy)
 * Picks the busiest thread, suspends it SAMPLES times (default 500, every 4000 us), walks its frame-pointer
 * chain and prints, per sample, "S <depth> <pc> <pc> ..." plus the loaded images ("I <base> <path>") for
 * symbolication on the host. Signed with task_for_pid-allow (tsample.plist).
 */
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
/* the iOS SDK hides libproc.h; these are in libSystem */
extern int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

static uint64_t
rd64(task_t t, uint64_t a)
{
	uint64_t v = 0;
	vm_size_t n = 0;
	if (vm_read_overwrite(t, a, 8, (vm_address_t)&v, &n) != KERN_SUCCESS || n != 8)
		return 0;
	return v;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: tsample PID [SAMPLES] [INTERVAL_US]\n");
		return 2;
	}
	int pid = atoi(argv[1]), samples = argc > 2 ? atoi(argv[2]) : 500, us = argc > 3 ? atoi(argv[3]) : 4000;
	if (argc > 4)
		sleep((unsigned)atoi(argv[4]));
	if (pid == 0) {
		static int pids[2048];
		int n = proc_listpids(1, 0, pids, sizeof(pids)) / (int)sizeof(int);
		for (int i = 0; i < n && pid == 0; i++) {
			char name[64] = "";
			if (pids[i] > 0 && proc_name(pids[i], name, sizeof(name)) > 0 && strcmp(name, argv[1]) == 0)
				pid = pids[i];
		}
		if (pid == 0) {
			fprintf(stderr, "tsample: no process %s\n", argv[1]);
			return 1;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	task_t task;
	kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "task_for_pid: %s\n", mach_error_string(kr));
		return 1;
	}
	int trig10 = argc > 5 ? atoi(argv[5]) * 10 : 0;     /* wait until the busiest thread used this % over 700 ms */
	thread_act_array_t th;
	mach_msg_type_number_t nth;
	int best, bestcpu;
	for (int tries = 0;; tries++) {
		if (task_threads(task, &th, &nth) != KERN_SUCCESS)
			return 1;
		/* the busiest thread: most CPU time used over the next 700 ms (cpu_usage is a slow average and misses a burst) */
		uint64_t t0[256];
		for (unsigned i = 0; i < nth && i < 256; i++) {
			thread_basic_info_data_t bi;
			mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
			t0[i] = 0;
			if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS)
				t0[i] = (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000 + bi.user_time.microseconds + bi.system_time.microseconds;
		}
		usleep(700000);
		best = -1;
		bestcpu = -1;
		for (unsigned i = 0; i < nth && i < 256; i++) {
			thread_basic_info_data_t bi;
			mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
			if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS) {
				uint64_t t1 = (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000 + bi.user_time.microseconds + bi.system_time.microseconds;
				int pct10 = (int)((t1 - t0[i]) / 700);        /* tenths of a percent of the 700 ms */
				if (pct10 > bestcpu) {
					bestcpu = pct10;
					best = i;
				}
			}
		}
		if (bestcpu >= trig10 || tries > 800)
			break;
		for (unsigned i = 0; i < nth; i++)
			mach_port_deallocate(mach_task_self(), th[i]);
		vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof(*th));
	}
	printf("T %d threads, busiest #%d at %.1f%%\n", nth, best, bestcpu / 10.0);
	thread_act_t t = th[best];
	for (int s = 0; s < samples; s++) {
		if (thread_suspend(t) != KERN_SUCCESS)
			break;
		arm_thread_state64_t st;
		mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
		uint64_t pcs[64];
		int d = 0;
		if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &c) == KERN_SUCCESS) {
			const uint64_t mask = 0x0000000fffffffffull;
			pcs[d++] = __darwin_arm_thread_state64_get_pc(st) & mask;
			pcs[d++] = __darwin_arm_thread_state64_get_lr(st) & mask;
			uint64_t fp = __darwin_arm_thread_state64_get_fp(st) & mask;
			while (fp && d < 64) {
				uint64_t next = rd64(task, fp) & mask, lr = rd64(task, fp + 8) & mask;
				if (!lr)
					break;
				pcs[d++] = lr;
				if (next <= fp)
					break;
				fp = next;
			}
		}
		thread_resume(t);
		printf("S %d", d);
		for (int i = 0; i < d; i++)
			printf(" %llx", pcs[i]);
		printf("\n");
		usleep(us);
	}
	struct task_dyld_info di;
	mach_msg_type_number_t c = TASK_DYLD_INFO_COUNT;
	if (task_info(task, TASK_DYLD_INFO, (task_info_t)&di, &c) == KERN_SUCCESS) {
		struct dyld_all_image_infos all;
		vm_size_t n = 0;
		if (vm_read_overwrite(task, di.all_image_info_addr, sizeof(all), (vm_address_t)&all, &n) == KERN_SUCCESS) {
			printf("C %llx\n", (unsigned long long)all.sharedCacheSlide);
			for (uint32_t i = 0; i < all.infoArrayCount; i++) {
				struct dyld_image_info ii;
				if (vm_read_overwrite(task, (vm_address_t)all.infoArray + i * sizeof(ii), sizeof(ii),
				    (vm_address_t)&ii, &n) != KERN_SUCCESS)
					continue;
				char path[512] = {0};
				vm_read_overwrite(task, (vm_address_t)ii.imageFilePath, sizeof(path) - 1, (vm_address_t)path, &n);
				printf("I %llx %s\n", (unsigned long long)(uintptr_t)ii.imageLoadAddress, path);
			}
		}
	}
	return 0;
}
