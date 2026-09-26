#include "vmm_types.h"
#include "vmm.h"
#include "mmu.h"

#include <inttypes.h>

int main(void) {
	// Initialize the TLB, frame tracking, and swap backing file
	vmm_init();

	// Create this process's page-table root and select its address space
	Process proc1 = {
		.asid = 1,
		.root = vmm_create_process_table()
	};
	current_asid = proc1.asid;

	uint64_t user_va_code = 0x000000400000;
	uint64_t user_va_rodata = 0x000000600000;
	uint64_t kernel_va = 0xFFFF80000000;

	// Map writable user memory, read-only user memory, and a global kernel page
	vmm_mmap(proc1.root, user_va_code, true, true, false);
	vmm_mmap(proc1.root, user_va_rodata, false, true, false);
	vmm_mmap(proc1.root, kernel_va, true, false, true);

	// First access allocates a physical frame for this virtual page
	printf("=== 1. Translating Writable User Page ===\n");
	uint64_t pa = mmu_translate(proc1.root, user_va_code, access_write, true);
	printf("Result PA: 0x%012" PRIX64 "\n", pa);

	// Repeating the translation should use the cached TLB entry.
	printf("\n=== 2. Re-accessing (TLB Hit Check) ===\n");
	pa = mmu_translate(proc1.root, user_va_code, access_read, true);
	printf("Result PA: 0x%012" PRIX64 "\n", pa);

	// Release the page tables & physical frames
	printf("\n=== 3. Cleaning Up Process Memory Tree ===\n");
	vmm_destroy_page_table(proc1.root, 4);

	vmm_cleanup();
	printf("Process Teardown Complete.\n");
	return 0;
}
