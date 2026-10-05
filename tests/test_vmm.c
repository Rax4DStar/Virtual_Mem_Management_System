#define _POSIX_C_SOURCE 200809L

#include "Frame_Alloc.h"
#include "mmu.h"
#include "vmm.h"

#include <sys/wait.h>
#include <unistd.h>

typedef void (*Test_Action)(void);

static void check(bool condition, const char *message) {
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(EXIT_FAILURE);
	}
}

static void start_process(Process *process, asid_t asid) {
	process->asid = asid;
	process->root = vmm_create_process_table();
	check(vmm_switch_process(process), "process switch should succeed");
}

static void check_mmu_result(MMU_Result actual, MMU_Result expected,
		const char *message) {
	if (actual != expected) {
		fprintf(stderr, "FAIL: %s (got %s, expected %s)\n", message,
			mmu_result_string(actual), mmu_result_string(expected));
		exit(EXIT_FAILURE);
	}
}

static void run_case(const char *name, Test_Action test) {
	fflush(stdout);
	pid_t child = fork();
	if (child < 0) {
		fprintf(stderr, "FAIL: could not start %s\n", name);
		exit(EXIT_FAILURE);
	}
	if (child == 0) {
		(void)freopen("/dev/null", "w", stdout);
		test();
		_exit(EXIT_SUCCESS);
	}

	int status = 0;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		WEXITSTATUS(status) != EXIT_SUCCESS) {
		fprintf(stderr, "FAIL: %s\n", name);
		exit(EXIT_FAILURE);
	}
	printf("PASS: %s\n", name);
}

static void test_frame_allocator(void) {
	physical_frame_allocator_init();
	int frames[max_frames];
	for (int i = 0; i < max_frames; i++) {
		frames[i] = physical_frame_alloc();
		check(frames[i] >= 0, "allocate every frame");
	}
	check(physical_frame_alloc() == -1, "report frame exhaustion");
	physical_free_frame(frames[12]);
	physical_free_frame(frames[12]); //a second free must not duplicate the slot
	check(physical_frame_alloc() == frames[12], "reuse the freed frame");
	check(physical_frame_alloc() == -1, "duplicate free must not add a frame");
}

static void test_fault_results_and_arguments(void) {
	char byte = 0;
	uint64_t physical_address;
	check_mmu_result(mmu_translate(0x400000, access_read, true, NULL),
		mmu_fault_invalid_argument, "translation requires an output address");
	check_mmu_result(mmu_read(0, NULL, 1, true), mmu_fault_invalid_argument,
		"non-empty read requires a buffer");
	check_mmu_result(mmu_write(UINT64_MAX, &byte, 2, true), mmu_fault_invalid_argument,
		"write range must not wrap around the address space");
	check_mmu_result(mmu_translate(0x400000, access_read, true, &physical_address),
		mmu_fault_unmapped, "translation without a selected process reports a fault");
}

static void test_process_isolation(void) {
	vmm_init();
	Process first;
	Process second;
	start_process(&first, 1);
	second.asid = 2;
	second.root = vmm_create_process_table();
	const uint64_t va = 0x400000;
	check(vmm_mmap(&first, va, true, false, true, false), "map first process");
	check(vmm_mmap(&second, va, true, false, true, false), "map second process");

	const char first_data[] = "first address space";
	const char second_data[] = "second address space";
	char buffer[sizeof(second_data)];
	check_mmu_result(mmu_write(va, first_data, sizeof(first_data), true),
		mmu_ok, "write first process data");
	check(vmm_switch_process(&second), "switch to second process");
	check_mmu_result(mmu_write(va, second_data, sizeof(second_data), true),
		mmu_ok, "write second process data");
	check(vmm_switch_process(&first), "switch to first process");
	check_mmu_result(mmu_read(va, buffer, sizeof(first_data), true),
		mmu_ok, "read first process data");
	check(memcmp(buffer, first_data, sizeof(first_data)) == 0, "process data stays isolated");
	check(vmm_switch_process(&second), "switch back to second process");
	check_mmu_result(mmu_read(va, buffer, sizeof(second_data), true),
		mmu_ok, "read second process data");
	check(memcmp(buffer, second_data, sizeof(second_data)) == 0, "second process data stays isolated");

	vmm_destroy_page_table(first.root, 4);
	vmm_destroy_page_table(second.root, 4);
	vmm_cleanup();
}

static void test_permissions(void) {
	vmm_init();
	Process process;
	start_process(&process, 3);
	const uint64_t va = 0x500000;
	check(vmm_mmap(&process, va, false, false, true, false),
		"map a read-only page");
	char byte;
	check_mmu_result(mmu_read(va, &byte, sizeof(byte), true), mmu_ok,
		"read from a read-only page");
	check_mmu_result(mmu_write(va, &byte, sizeof(byte), true), mmu_fault_write_permission,
		"write to a read-only page reports a fault");
	uint64_t physical_address;
	check_mmu_result(mmu_translate(va, access_exec, true, &physical_address),
		mmu_fault_execute_permission, "execute from a non-executable page reports a fault");

	check(vmm_mmap(&process, va, true, false, false, false),
		"remap page as kernel-only");
	check_mmu_result(mmu_read(va, &byte, sizeof(byte), true), mmu_fault_privilege,
		"user read from kernel page reports a fault");
	vmm_destroy_page_table(process.root, 4);
	vmm_cleanup();
}

static void test_mapping_lifecycle(void) {
	vmm_init();
	Process process;
	start_process(&process, 4);
	const uint64_t va = 0x700000;
	check(vmm_mmap(&process, va, true, false, true, false), "create initial mapping");
	const char data[] = "old contents";
	check_mmu_result(mmu_write(va, data, sizeof(data), true), mmu_ok,
		"write before remapping");
	check(vmm_mmap(&process, va, false, false, true, false), "replace mapping permissions");
	char zeroed[sizeof(data)];
	check_mmu_result(mmu_read(va, zeroed, sizeof(zeroed), true), mmu_ok,
		"read remapped page");
	for (size_t i = 0; i < sizeof(zeroed); i++) {
		check(zeroed[i] == 0, "remapping starts with a fresh zero-filled page");
	}
	check_mmu_result(mmu_write(va, data, sizeof(data), true), mmu_fault_write_permission,
		"remapped read-only permissions must reach the TLB");
	check(vmm_munmap(&process, va), "unmap existing page");
	check(!vmm_munmap(&process, va), "unmapping twice reports no mapping");
	char byte;
	check_mmu_result(mmu_read(va, &byte, sizeof(byte), true), mmu_fault_unmapped,
		"access after unmap reports an unmapped fault");
	check(vmm_mmap(&process, va, true, false, true, false), "map again after unmap");
	check_mmu_result(mmu_write(va, data, sizeof(data), true), mmu_ok,
		"write after mapping again");
	check(vmm_munmap(&process, va), "unmap remapped page");
	vmm_destroy_page_table(process.root, 4);
	vmm_cleanup();
}

static void test_cross_page_bytes(void) {
	vmm_init();
	Process process;
	start_process(&process, 5);
	const uint64_t base = 0x800000;
	check(vmm_mmap(&process, base, true, false, true, false), "map first transfer page");
	check(vmm_mmap(&process, base + page_size, true, false, true, false), "map second transfer page");
	const char expected[] = "transfer spans two virtual pages";
	char actual[sizeof(expected)];
	uint64_t va = base + page_size - 4;
	check_mmu_result(mmu_write(va, expected, sizeof(expected), true), mmu_ok,
		"write across page boundary");
	check_mmu_result(mmu_read(va, actual, sizeof(actual), true), mmu_ok,
		"read across page boundary");
	check(memcmp(actual, expected, sizeof(expected)) == 0, "cross-page data matches");
	vmm_destroy_page_table(process.root, 4);
	vmm_cleanup();
}

static void test_swap_preserves_bytes(void) {
	vmm_init();
	Process process;
	start_process(&process, 6);
	const uint64_t base = 0x10000000;
	for (int i = 0; i <= max_frames; i++) {
		check(vmm_mmap(&process, base + (uint64_t)i * page_size,
			true, false, true, false), "map page for swap test");
	}

	const char expected[] = "saved across page eviction";
	char actual[sizeof(expected)];
	check_mmu_result(mmu_write(base, expected, sizeof(expected), true), mmu_ok,
		"write page before eviction");
	for (int i = 1; i <= max_frames; i++) {
		uint64_t physical_address;
		check_mmu_result(mmu_translate(base + (uint64_t)i * page_size,
			access_read, true, &physical_address), mmu_ok, "touch page for eviction");
	}
	check_mmu_result(mmu_read(base, actual, sizeof(actual), true), mmu_ok,
		"read evicted page back in");
	check(memcmp(actual, expected, sizeof(expected)) == 0, "swap preserves page bytes");
	vmm_destroy_page_table(process.root, 4);
	vmm_cleanup();
}

int main(void) {
	run_case("frame allocator", test_frame_allocator);
	run_case("fault results and invalid arguments", test_fault_results_and_arguments);
	run_case("process isolation and switching", test_process_isolation);
	run_case("read, write, execute, and privilege permissions", test_permissions);
	run_case("remap, unmap, and map again", test_mapping_lifecycle);
	run_case("byte access across page boundary", test_cross_page_bytes);
	run_case("swap preserves written bytes", test_swap_preserves_bytes);
	puts("All tests passed.");
	return EXIT_SUCCESS;
}
