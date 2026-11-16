#include "mmu.h"
#include "vmm.h"

#include <inttypes.h>

#define benchmark_extra_pages 64
#define hot_page_start 16
#define hot_page_count 8

static bool touch_page(uint64_t va) {
	uint64_t physical_address;
	return mmu_translate(va, access_read, true, &physical_address) == mmu_ok;
}

static bool run_workload(VMM_Replacement_Policy policy, VMM_Stats *result) {
	const uint64_t base = 0x50000000;
	const int page_count = max_frames + benchmark_extra_pages;
	Process process = {
		.asid = (asid_t)(policy + 1),
		.root = NULL
	};
	vmm_init();
	process.root = vmm_create_process_table();
	if (!vmm_set_replacement_policy(policy) || !vmm_switch_process(&process)) {
		vmm_destroy_page_table(process.root, 4);
		vmm_cleanup();
		return false;
	}

	for (int i = 0; i < page_count; i++) {
		if (!vmm_mmap(&process, base + (uint64_t)i * page_size,
				true, false, true, false)) {
			vmm_destroy_page_table(process.root, 4);
			vmm_cleanup();
			return false;
		}
	}

	// Fill RAM, then repeatedly reuse a small hot set while introducing cold pages.
	for (int i = 0; i < max_frames; i++) {
		if (!touch_page(base + (uint64_t)i * page_size)) goto fail;
	}
	for (int i = max_frames; i < page_count; i++) {
		for (int hot = 0; hot < hot_page_count; hot++) {
			uint64_t hot_va = base + (uint64_t)(hot_page_start + hot) * page_size;
			if (!touch_page(hot_va)) goto fail;
		}
		if (!touch_page(base + (uint64_t)i * page_size)) goto fail;
	}

	*result = vmm_get_stats();
	vmm_destroy_page_table(process.root, 4);
	vmm_cleanup();
	return true;

fail:
	vmm_destroy_page_table(process.root, 4);
	vmm_cleanup();
	return false;
}

int main(void) {
	VMM_Stats fifo_stats;
	VMM_Stats clock_stats;
	if (!run_workload(vmm_replacement_fifo, &fifo_stats) ||
		!run_workload(vmm_replacement_clock, &clock_stats)) {
		fprintf(stderr, "Failed to run replacement benchmark workload.\n");
		return EXIT_FAILURE;
	}

	printf("Policy comparison: %d frames, %d hot pages, %d new pages\n",
		max_frames, hot_page_count, benchmark_extra_pages);
	printf("%-8s %12s %12s %14s %12s %12s\n", "Policy",
		"Page faults", "Evictions", "Clock checks", "TLB hits", "TLB misses");
	printf("%-8s %12" PRIu64 " %12" PRIu64 " %14" PRIu64
		" %12" PRIu64 " %12" PRIu64 "\n", "FIFO",
		fifo_stats.page_faults, fifo_stats.page_evictions,
		fifo_stats.clock_page_checks, fifo_stats.tlb_hits,
		fifo_stats.tlb_misses);
	printf("%-8s %12" PRIu64 " %12" PRIu64 " %14" PRIu64
		" %12" PRIu64 " %12" PRIu64 "\n", "Clock",
		clock_stats.page_faults, clock_stats.page_evictions,
		clock_stats.clock_page_checks, clock_stats.tlb_hits,
		clock_stats.tlb_misses);
	return EXIT_SUCCESS;
}
