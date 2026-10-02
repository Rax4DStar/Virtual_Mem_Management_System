#include "vmm_types.h"
#include "vmm.h"
#include "mmu.h"

#include <inttypes.h>

int main(void) {
	// Initialize the TLB, frame tracking, and swap backing file
	vmm_init();

	// Create two processes with separate page tables and ASIDs.
	Process proc1 = {
		.asid = 1,
		.root = vmm_create_process_table()
	};
	Process proc2 = {
		.asid = 2,
		.root = vmm_create_process_table()
	};
	if (!vmm_switch_process(&proc1)) return EXIT_FAILURE;

	uint64_t user_va_code = 0x000000400000;
	uint64_t user_va_rodata = 0x000000600000;
	uint64_t kernel_va = 0xFFFF80000000;

	// Map writable user memory, read-only user memory, and a global kernel page
	vmm_mmap(&proc1, user_va_code, true, true, true, false);
	vmm_mmap(&proc1, user_va_code + page_size, true, false, true, false);
	vmm_mmap(&proc1, user_va_rodata, false, false, true, false);
	vmm_mmap(&proc1, kernel_va, true, false, false, true);
	// Both processes use the same VA, but each owns a separate mapping.
	vmm_mmap(&proc2, user_va_code, true, false, true, false);

	// First access allocates a physical frame for this virtual page
	printf("=== 1. Translating Writable User Page ===\n");
	const char proc1_message[] = "data owned by process 1";
	char proc1_readback[sizeof(proc1_message)];
	if (!mmu_write(user_va_code, proc1_message, sizeof(proc1_message), true) ||
		!mmu_read(user_va_code, proc1_readback, sizeof(proc1_readback), true)) {
		return EXIT_FAILURE;
	}
	printf("Process 1 read: %s\n", proc1_readback);
	const char spanning_message[] = "cross-page transfer";
	char spanning_readback[sizeof(spanning_message)];
	uint64_t spanning_va = user_va_code + page_size - 3;
	if (!mmu_write(spanning_va, spanning_message, sizeof(spanning_message), true) ||
		!mmu_read(spanning_va, spanning_readback, sizeof(spanning_readback), true)) {
		return EXIT_FAILURE;
	}
	printf("Cross-page read: %s\n", spanning_readback);
	uint64_t pa = mmu_translate(user_va_code, access_write, true);
	printf("Result PA: 0x%012" PRIX64 "\n", pa);

	// Switching ASIDs should select proc2's independent mapping at the same VA.
	printf("\n=== 2. Switching Process ===\n");
	if (!vmm_switch_process(&proc2)) return EXIT_FAILURE;
	const char proc2_message[] = "data owned by process 2";
	char proc2_readback[sizeof(proc2_message)];
	if (!mmu_write(user_va_code, proc2_message, sizeof(proc2_message), true) ||
		!mmu_read(user_va_code, proc2_readback, sizeof(proc2_readback), true)) {
		return EXIT_FAILURE;
	}
	printf("Process 2 read: %s\n", proc2_readback);
	uint64_t proc2_pa = mmu_translate(user_va_code, access_read, true);
	printf("Process 2 PA: 0x%012" PRIX64 "\n", proc2_pa);

	// Switching back must select proc1's mapping for the same virtual address.
	printf("\n=== 3. Switching Back (ASID Check) ===\n");
	if (!vmm_switch_process(&proc1)) return EXIT_FAILURE;
	if (!mmu_read(user_va_code, proc1_readback, sizeof(proc1_readback), true)) {
		return EXIT_FAILURE;
	}
	printf("Process 1 still reads: %s\n", proc1_readback);
	pa = mmu_translate(user_va_code, access_read, true);
	printf("Result PA: 0x%012" PRIX64 "\n", pa);

	// Replace an existing mapping, then remove it and map the VA again.
	printf("\n=== 4. Remapping and Unmapping a Page ===\n");
	if (!vmm_mmap(&proc1, user_va_rodata, true, false, true, false)) {
		return EXIT_FAILURE;
	}
	pa = mmu_translate(user_va_rodata, access_write, true);
	printf("Remapped PA: 0x%012" PRIX64 "\n", pa);
	if (!vmm_munmap(&proc1, user_va_rodata) ||
		!vmm_mmap(&proc1, user_va_rodata, false, false, true, false)) {
		return EXIT_FAILURE;
	}
	pa = mmu_translate(user_va_rodata, access_read, true);
	printf("Mapped again PA: 0x%012" PRIX64 "\n", pa);

	// Release the page tables & physical frames
	printf("\n=== 5. Cleaning Up Process Memory Trees ===\n");
	vmm_destroy_page_table(proc1.root, 4);
	vmm_destroy_page_table(proc2.root, 4);

	vmm_cleanup();
	printf("Process Teardown Complete.\n");
	return 0;
}
