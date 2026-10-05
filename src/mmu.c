#include "vmm_types.h"
#include "mmu.h"
#include "tlb.h"
#include "vmm.h"

static MMU_Result check_permissions(Access_Type access_type, bool is_user,
		bool readable, bool writeable, bool executable, bool user_mode) {
	if ((access_type & access_read) && !readable) return mmu_fault_read_permission;
	if ((access_type & access_write) && !writeable) return mmu_fault_write_permission;
	if ((access_type & access_exec) && !executable) return mmu_fault_execute_permission;
	if (is_user && !user_mode) return mmu_fault_privilege;
	return mmu_ok;
}

MMU_Result mmu_translate(uint64_t va, Access_Type access_type, bool is_user,
		uint64_t *physical_address) {
	if (!physical_address) return mmu_fault_invalid_argument;
	if (!current_process || !current_process->root) return mmu_fault_unmapped;

	uint64_t vpn = va >> 12;
	uint16_t offset = get_offset(va);
	Table_Node *l4_root = current_process->root;
	asid_t asid = current_process->asid;

	TLB_Entry cached;
	if (tlb_lookup(vpn, asid, &cached)) {
		MMU_Result permission = check_permissions(access_type, is_user,
			cached.readable, cached.writeable, cached.executable, cached.user_mode);
		if (permission != mmu_ok) return permission;

		if (cached.pte) {
			cached.pte->accessed = true;
			if (access_type & access_write) cached.pte->dirty = true;
		}
		*physical_address = cached.frame_base | offset;
		return mmu_ok;
	}

	// Missing intermediate tables mean the virtual address was never mapped.
	uint16_t l4 = get_L4(va);
	if (!l4_root->entries[l4].present) return mmu_fault_unmapped;
	Table_Node *l3_table = (Table_Node *)l4_root->entries[l4].address;

	uint16_t l3 = get_L3(va);
	if (!l3_table->entries[l3].present) return mmu_fault_unmapped;
	Table_Node *l2_table = (Table_Node *)l3_table->entries[l3].address;

	uint16_t l2 = get_L2(va);
	if (!l2_table->entries[l2].present) return mmu_fault_unmapped;
	Table_Node *l1_table = (Table_Node *)l2_table->entries[l2].address;

	uint16_t l1 = get_L1(va);
	Page_Entry *pte = &l1_table->entries[l1];
	if (!pte->mapped) return mmu_fault_unmapped;

	// Check permissions before paging in a non-resident page.
	MMU_Result permission = check_permissions(access_type, is_user,
		pte->readable, pte->writeable, pte->executable, pte->user_mode);
	if (permission != mmu_ok) return permission;

	if (!pte->present) {
		vmm_handle_page_fault(l4_root, va, access_type);
	}

	pte->accessed = true;
	if (access_type & access_write) pte->dirty = true;
	tlb_insert(vpn, pte, asid);
	*physical_address = pte->address | offset;
	return mmu_ok;
}

static MMU_Result mmu_transfer(uint64_t va, void *buffer, size_t size,
		Access_Type access_type, bool is_user) {
	if (size == 0) return mmu_ok;
	if (!buffer || size - 1 > UINT64_MAX - va) return mmu_fault_invalid_argument;

	uint8_t *bytes = (uint8_t *)buffer;
	while (size > 0) {
		// Copy only to the end of this page, then translate the next one.
		size_t chunk = page_size - get_offset(va);
		if (chunk > size) chunk = size;

		uint64_t physical_address;
		MMU_Result result = mmu_translate(va, access_type, is_user, &physical_address);
		if (result != mmu_ok) return result;

		bool copied = access_type == access_read
			? vmm_read_physical(physical_address, bytes, chunk)
			: vmm_write_physical(physical_address, bytes, chunk);
		if (!copied) return mmu_fault_invalid_physical_address;

		va += chunk;
		bytes += chunk;
		size -= chunk;
	}
	return mmu_ok;
}

MMU_Result mmu_read(uint64_t va, void *buffer, size_t size, bool is_user) {
	return mmu_transfer(va, buffer, size, access_read, is_user);
}

MMU_Result mmu_write(uint64_t va, const void *buffer, size_t size, bool is_user) {
	// The shared transfer routine reads from this buffer but never changes it.
	return mmu_transfer(va, (void *)buffer, size, access_write, is_user);
}

const char *mmu_result_string(MMU_Result result) {
	switch (result) {
	case mmu_ok: return "success";
	case mmu_fault_unmapped: return "unmapped address";
	case mmu_fault_read_permission: return "read permission denied";
	case mmu_fault_write_permission: return "write permission denied";
	case mmu_fault_execute_permission: return "execute permission denied";
	case mmu_fault_privilege: return "privilege violation";
	case mmu_fault_invalid_argument: return "invalid memory access argument";
	case mmu_fault_invalid_physical_address: return "invalid physical address";
	}
	return "unknown MMU result";
}
