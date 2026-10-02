#include "vmm_types.h"
#include "mmu.h"
#include "tlb.h"
#include "vmm.h"
#include <inttypes.h>

static void trigger_segfault(uint64_t va, const char *reason) {
	printf("\n [MMU HARDWARE FAULT] SIGSEGV at VA 0x%012" PRIX64 " -> Reason: %s\n", va, reason);
	exit(139);
} //terminator for hardware faults

uint64_t mmu_translate(uint64_t va, Access_Type access_type, bool is_user) {
	uint64_t vpn = va >> 12;
	uint16_t offset = get_offset(va); //get vpn and offset
	if (!current_process || !current_process->root) {
		trigger_segfault(va, "Unmapped Address");
	}
	Table_Node *l4_root = current_process->root;
	asid_t asid = current_process->asid;

	TLB_Entry cached;
	if (tlb_lookup(vpn, asid, &cached)) { //check TLB hit, including cached permissions
		if ((access_type & access_read) && !cached.readable) {
			trigger_segfault(va, "Read Access Violation");
		}
		if ((access_type & access_write) && !cached.writeable) {
			trigger_segfault(va, "Write Access Violation");
		}
		if ((access_type & access_exec) && !cached.executable) {
			trigger_segfault(va, "Execute Access Violation");
		}
		if (is_user && !cached.user_mode) {
			trigger_segfault(va, "Privilege Violation");
		}
		if (cached.pte) {
			cached.pte->accessed = true;
			if (access_type & access_write) cached.pte->dirty = true;
		}
		printf("[MMU] TLB HIT! ");
		return cached.frame_base | offset;
	}

	uint16_t l4 = get_L4(va);
	// A missing intermediate table means this VA was never mapped; the fault
	// handler can only populate leaf pages beneath an existing hierarchy.
	if (!l4_root->entries[l4].present) {
		trigger_segfault(va, "Unmapped Address");
	}
	Table_Node *l3_table = (Table_Node *)l4_root->entries[l4].address;

	uint16_t l3 = get_L3(va);
	if (!l3_table->entries[l3].present) {
		trigger_segfault(va, "Unmapped Address");
	}
	Table_Node *l2_table = (Table_Node *)l3_table->entries[l3].address;

	uint16_t l2 = get_L2(va);
	if (!l2_table->entries[l2].present) {
		trigger_segfault(va, "Unmapped Address");
	}
	Table_Node *l1_table = (Table_Node *)l2_table->entries[l2].address;

	uint16_t l1 = get_L1(va);
	Page_Entry *pte = &l1_table->entries[l1]; //get pte via L1 

    //above marks a TBL miss walk seqeunce for our MMU.

	if (!pte->mapped) {
		trigger_segfault(va, "Unmapped Address");
	}
	if (!pte->present) { //check if our page exists
		vmm_handle_page_fault(l4_root, va, access_type);
	}

	if ((access_type & access_read) && !pte->readable) {
		trigger_segfault(va, "Read Access Violation");
	}
	if ((access_type & access_write) && !pte->writeable) {
		trigger_segfault(va, "Write Access Violation");
	}
	if ((access_type & access_exec) && !pte->executable) {
		trigger_segfault(va, "Execute Access Violation");
	}
	if (is_user && !pte->user_mode) {
		trigger_segfault(va, "Privilege Violation");
	}
    //above we check several permissions ensuring its valid/permitted 

	pte->accessed = true;
	if (access_type & access_write) pte->dirty = true;  //mark page as being used/dirty

	tlb_insert(vpn, pte, asid); //cache the translation and permissions for future hits

	return pte->address | offset; //return physical address
}

static bool mmu_transfer(uint64_t va, void *buffer, size_t size,
		Access_Type access_type, bool is_user) {
	if (size == 0) return true;
	if (!buffer || size - 1 > UINT64_MAX - va) return false;

	uint8_t *bytes = (uint8_t *)buffer;
	while (size > 0) {
		// Copy only to the end of this page, then translate the next one.
		size_t chunk = page_size - get_offset(va);
		if (chunk > size) chunk = size;

		uint64_t physical_address = mmu_translate(va, access_type, is_user);
		bool copied = access_type == access_read
			? vmm_read_physical(physical_address, bytes, chunk)
			: vmm_write_physical(physical_address, bytes, chunk);
		if (!copied) return false;

		va += chunk;
		bytes += chunk;
		size -= chunk;
	}
	return true;
}

bool mmu_read(uint64_t va, void *buffer, size_t size, bool is_user) {
	return mmu_transfer(va, buffer, size, access_read, is_user);
}

bool mmu_write(uint64_t va, const void *buffer, size_t size, bool is_user) {
	// The shared transfer routine does not modify the input bytes.
	return mmu_transfer(va, (void *)buffer, size, access_write, is_user);
}
