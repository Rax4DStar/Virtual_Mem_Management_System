#include "vmm_types.h"
#include "vmm.h"
#include "Frame_Alloc.h"
#include "tlb.h"
#include "node.h"

#include <inttypes.h>
#include <limits.h>

static FrameTracker frame_table[max_frames];
Process *current_process = NULL;

typedef struct {
	Process *process;
	Table_Node *root;
	asid_t asid;
} Process_Registration;

typedef struct {
	uint64_t vpn;
	Process *owner;
	Page_Entry *pte;
	uint8_t state; //0 empty, 1 occupied, 2 deleted
} Global_Mapping;

#define global_mapping_capacity (max_table_nodes * max_entries)
static Process_Registration process_registry[max_table_nodes];
static Global_Mapping global_mappings[global_mapping_capacity];
/* Translation combines a frame base with the page offset, so every frame must
 begin at an address whose low page-offset bits are zero */
_Static_assert((page_size & (page_size - 1)) == 0,
	"page_size must be a power of two for aligned frame addressing");
static _Alignas(page_size) uint8_t physical_memory[max_frames][page_size];
static FILE *swap_file = NULL;
// Resident frames form a FIFO queue so victim selection does not scan frames
static int16_t replacement_head = -1;
static int16_t replacement_tail = -1;
// A mapped page can own at most one swap slot.
static uint64_t free_swap_offsets[max_table_nodes * max_entries];
static size_t free_swap_count = 0;
static uint64_t next_swap_offset = 0;

static bool swap_slot_allocate(uint64_t *offset) {
	if (free_swap_count > 0) {
		*offset = free_swap_offsets[--free_swap_count];
		return true;
	}

	if (next_swap_offset > (uint64_t)LONG_MAX - page_size) return false;
	*offset = next_swap_offset;
	next_swap_offset += page_size;
	return true;
}

static void swap_slot_release(uint64_t offset) {
	if (offset % page_size != 0 ||
		free_swap_count >= max_table_nodes * max_entries) {
		fprintf(stderr, "[VMM Error] Invalid or excessive free swap slots.\n");
		exit(EXIT_FAILURE);
	}
	free_swap_offsets[free_swap_count++] = offset;
}

static bool swap_seek(uint64_t offset) {
	return offset <= (uint64_t)LONG_MAX &&
		fseek(swap_file, (long)offset, SEEK_SET) == 0;
}

static bool register_process(Process *process) {
	int free_slot = -1;
	for (int i = 0; i < max_table_nodes; i++) {
		Process_Registration *entry = &process_registry[i];
		if (!entry->process) {
			if (free_slot < 0) free_slot = i;
			continue;
		}
		if (entry->process == process) {
			return entry->root == process->root && entry->asid == process->asid;
		}
		if (entry->asid == process->asid || entry->root == process->root) return false;
	}

	if (free_slot < 0) return false;
	process_registry[free_slot] = (Process_Registration){
		.process = process,
		.root = process->root,
		.asid = process->asid
	};
	return true;
}

static Global_Mapping *find_global_mapping(uint64_t vpn) {
	size_t index = (size_t)(vpn % global_mapping_capacity);
	for (size_t probe = 0; probe < global_mapping_capacity; probe++) {
		Global_Mapping *entry = &global_mappings[index];
		if (entry->state == 0) return NULL;
		if (entry->state == 1 && entry->vpn == vpn) return entry;
		index = (index + 1) % global_mapping_capacity;
	}
	return NULL;
}

static bool add_global_mapping(uint64_t vpn, Process *owner, Page_Entry *pte) {
	size_t index = (size_t)(vpn % global_mapping_capacity);
	size_t first_deleted = global_mapping_capacity;
	for (size_t probe = 0; probe < global_mapping_capacity; probe++) {
		Global_Mapping *entry = &global_mappings[index];
		if (entry->state == 1 && entry->vpn == vpn) {
			if (entry->owner != owner) return false;
			entry->pte = pte;
			return true;
		}
		if (entry->state == 2 && first_deleted == global_mapping_capacity) {
			first_deleted = index;
		}
		if (entry->state == 0) {
			if (first_deleted != global_mapping_capacity) index = first_deleted;
			global_mappings[index] = (Global_Mapping){
				.vpn = vpn,
				.owner = owner,
				.pte = pte,
				.state = 1
			};
			return true;
		}
		index = (index + 1) % global_mapping_capacity;
	}

	if (first_deleted != global_mapping_capacity) {
		global_mappings[first_deleted] = (Global_Mapping){
			.vpn = vpn,
			.owner = owner,
			.pte = pte,
			.state = 1
		};
		return true;
	}
	return false;
}

static void remove_global_mapping(uint64_t vpn, Process *owner) {
	Global_Mapping *entry = find_global_mapping(vpn);
	if (entry && entry->owner == owner) {
		tlb_invalidate_vpn(vpn);
		entry->owner = NULL;
		entry->pte = NULL;
		entry->state = 2;
	}
}

static Page_Entry *find_page_entry(Table_Node *root, uint64_t va) {
	uint16_t l4 = get_L4(va);
	uint16_t l3 = get_L3(va);
	uint16_t l2 = get_L2(va);
	uint16_t l1 = get_L1(va);
	if (!root->entries[l4].present) return NULL;
	Table_Node *l3_table = (Table_Node *)root->entries[l4].address;
	if (!l3_table->entries[l3].present) return NULL;
	Table_Node *l2_table = (Table_Node *)l3_table->entries[l3].address;
	if (!l2_table->entries[l2].present) return NULL;
	Table_Node *l1_table = (Table_Node *)l2_table->entries[l2].address;
	return &l1_table->entries[l1];
}

static void unregister_process_root(Table_Node *root) {
	for (int i = 0; i < max_table_nodes; i++) {
		Process_Registration *entry = &process_registry[i];
		if (entry->process && entry->root == root) {
			if (current_process == entry->process) current_process = NULL;
			entry->process = NULL;
			entry->root = NULL;
		}
	}
	for (size_t i = 0; i < global_mapping_capacity; i++) {
		Global_Mapping *entry = &global_mappings[i];
		if (entry->state == 1 && entry->owner && entry->owner->root == root) {
			tlb_invalidate_vpn(entry->vpn);
			entry->owner = NULL;
			entry->pte = NULL;
			entry->state = 2;
		}
	}
}

static void replacement_remove(int frame_idx) {
	FrameTracker *frame = &frame_table[frame_idx];
	if (frame->replacement_prev >= 0) {
		frame_table[frame->replacement_prev].replacement_next = frame->replacement_next;
	} else {
		replacement_head = frame->replacement_next;
	}
	if (frame->replacement_next >= 0) {
		frame_table[frame->replacement_next].replacement_prev = frame->replacement_prev;
	} else {
		replacement_tail = frame->replacement_prev;
	}
	frame->replacement_prev = -1;
	frame->replacement_next = -1;
}

static void replacement_append(int frame_idx) {
	FrameTracker *frame = &frame_table[frame_idx];
	frame->replacement_prev = replacement_tail;
	frame->replacement_next = -1;
	if (replacement_tail >= 0) {
		frame_table[replacement_tail].replacement_next = (int16_t)frame_idx;
	} else {
		replacement_head = (int16_t)frame_idx;
	}
	replacement_tail = (int16_t)frame_idx;
}

static void release_page_mapping(uint64_t va, Page_Entry *pte) {
	if (pte->present && pte->frame_index >= 0 && pte->frame_index < max_frames) {
		int frame_idx = pte->frame_index;
		FrameTracker *frame = &frame_table[frame_idx];
		if (frame->busy && frame->pte == pte) {
			replacement_remove(frame_idx);
			physical_free_frame(frame_idx);
			frame->busy = false;
			frame->pte = NULL;
			frame->owner_va = 0;
			frame->owner_asid = 0;
		}
	}
	if (pte->swapped) swap_slot_release(pte->swap_offset);

	// Remove any cached copy before clearing the page entry.
	tlb_invalidate_vpn(va >> 12);
	memset(pte, 0, sizeof(*pte));
	pte->frame_index = -1;
}

void vmm_init(void) {
	// Start with an empty TLB and reset frame ownership
	current_process = NULL;
	memset(process_registry, 0, sizeof(process_registry));
	memset(global_mappings, 0, sizeof(global_mappings));
	tlb_init();
	physical_frame_allocator_init();
	free_swap_count = 0;
	next_swap_offset = 0;
	swap_file = fopen("swap.bin", "w+b");
	if (!swap_file) {
		perror("Failed to open swap.bin");
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < max_frames; i++) {
		frame_table[i].busy = false;
		frame_table[i].pte = NULL;
		frame_table[i].owner_va = 0;
		frame_table[i].owner_asid = 0;
		frame_table[i].replacement_prev = -1;
		frame_table[i].replacement_next = -1;
	}
	replacement_head = -1;
	replacement_tail = -1;
}

void vmm_cleanup(void) {
	// Close the backing store when the VMM shuts down
	if (swap_file) {
		fclose(swap_file);
		swap_file = NULL;
	}
}

Table_Node *vmm_create_process_table(void) {
	// Get a cleared node from the pool.
	Table_Node *node = alloc_node();
	if (!node) {
		printf("[VMM Error] Out of static page-table nodes.\n");
		exit(EXIT_FAILURE);
	}
	return node;
}

bool vmm_switch_process(Process *process) {
	// Keep the root table and ASID together when changing address spaces.
	if (!process || !process->root) return false;
	if (!register_process(process)) return false;
	current_process = process;
	return true;
}

bool vmm_mmap(Process *process, uint64_t va, bool writable, bool executable,
		bool user_mode, bool global_page) {
	if (!process || !process->root) return false;
	if (!register_process(process)) return false;
	uint64_t vpn = va >> 12;
	Global_Mapping *registered_global = find_global_mapping(vpn);
	if (registered_global && registered_global->owner != process) return false;
	if (registered_global && !global_page) return false;
	if (global_page && !registered_global) {
		for (int i = 0; i < max_table_nodes; i++) {
			Process_Registration *entry = &process_registry[i];
			if (!entry->process || entry->process == process) continue;
			Page_Entry *other_pte = find_page_entry(entry->root, va);
			if (other_pte && other_pte->mapped) return false;
		}
	}
	Table_Node *root = process->root;

	// Create each level so VA reaches a leaf entry
	uint16_t l4 = get_L4(va);
	uint16_t l3 = get_L3(va);
	uint16_t l2 = get_L2(va);
	uint16_t l1 = get_L1(va);

	if (!root->entries[l4].present) {
		root->entries[l4].address = (uint64_t)vmm_create_process_table();
		root->entries[l4].present = true;
	}
	Table_Node *l3_table = (Table_Node *)root->entries[l4].address;

	if (!l3_table->entries[l3].present) {
		l3_table->entries[l3].address = (uint64_t)vmm_create_process_table();
		l3_table->entries[l3].present = true;
	}
	Table_Node *l2_table = (Table_Node *)l3_table->entries[l3].address;

	if (!l2_table->entries[l2].present) {
		l2_table->entries[l2].address = (uint64_t)vmm_create_process_table();
		l2_table->entries[l2].present = true;
	}
	Table_Node *l1_table = (Table_Node *)l2_table->entries[l2].address;

	Page_Entry *pte = &l1_table->entries[l1];
	if (global_page && !add_global_mapping(vpn, process, pte)) return false;
	if (pte->mapped) {
		// Release the old frame and translation before changing the mapping.
		release_page_mapping(va, pte);
	}

	// Record permissions now.Physical page is allocated on first access
	pte->mapped = true;
	pte->address = 0;
	pte->present = false;
	pte->frame_index = -1;
	pte->readable = true;
	pte->writeable = writable;
	pte->executable = executable;
	pte->user_mode = user_mode;
	pte->global_page = global_page;
	pte->accessed = false;
	pte->dirty = false;
	pte->swapped = false;
	pte->swap_offset = 0;
	return true;
}

static bool table_is_empty(Table_Node *table, int level) {
	for (int i = 0; i < max_entries; i++) {
		if (level == 1 ? table->entries[i].mapped : table->entries[i].present) {
			return false;
		}
	}
	return true;
}

bool vmm_munmap(Process *process, uint64_t va) {
	if (!process || !process->root) return false;

	uint16_t l4 = get_L4(va);
	uint16_t l3 = get_L3(va);
	uint16_t l2 = get_L2(va);
	uint16_t l1 = get_L1(va);
	Table_Node *root = process->root;
	if (!root->entries[l4].present) return false;
	Table_Node *l3_table = (Table_Node *)root->entries[l4].address;
	if (!l3_table->entries[l3].present) return false;
	Table_Node *l2_table = (Table_Node *)l3_table->entries[l3].address;
	if (!l2_table->entries[l2].present) return false;
	Table_Node *l1_table = (Table_Node *)l2_table->entries[l2].address;
	Page_Entry *pte = &l1_table->entries[l1];
	if (!pte->mapped) return false;
	bool was_global = pte->global_page;

	// Release a resident frame and clear any cached translation.
	release_page_mapping(va, pte);
	if (was_global) remove_global_mapping(va >> 12, process);

	// Return page-table nodes that no longer contain mappings.
	if (table_is_empty(l1_table, 1)) {
		free_node(l1_table);
		memset(&l2_table->entries[l2], 0, sizeof(l2_table->entries[l2]));
	}
	if (table_is_empty(l2_table, 2)) {
		free_node(l2_table);
		memset(&l3_table->entries[l3], 0, sizeof(l3_table->entries[l3]));
	}
	if (table_is_empty(l3_table, 3)) {
		free_node(l3_table);
		memset(&root->entries[l4], 0, sizeof(root->entries[l4]));
	}
	return true;
}

static uint8_t *physical_memory_pointer(uint64_t address, size_t size) {
	uintptr_t memory_start = (uintptr_t)&physical_memory[0][0];
	uintptr_t memory_end = memory_start + sizeof(physical_memory);
	if (address < memory_start || address > memory_end ||
		size > memory_end - (uintptr_t)address) {
		return NULL;
	}
	return (uint8_t *)(uintptr_t)address;
}

bool vmm_read_physical(uint64_t address, void *buffer, size_t size) {
	if (size == 0) return true;
	uint8_t *source = physical_memory_pointer(address, size);
	if (!source || !buffer) return false;
	memcpy(buffer, source, size);
	return true;
}

bool vmm_write_physical(uint64_t address, const void *buffer, size_t size) {
	if (size == 0) return true;
	uint8_t *destination = physical_memory_pointer(address, size);
	if (!destination || !buffer) return false;
	memcpy(destination, buffer, size);
	return true;
}

void vmm_destroy_page_table(Table_Node *table, int level) {
	if (!table) return;
	if (level == 4) unregister_process_root(table);

	for (int i = 0; i < max_entries; i++) {
		Page_Entry *entry = &table->entries[i];

		if (level == 1) {
			// Unmapped swapped pages still own a slot in the backing file.
			if (entry->mapped && entry->swapped) {
				swap_slot_release(entry->swap_offset);
			}
			if (!entry->present) continue;

			// The PTE records its resident frame, avoiding a scan of all frames
			int frame_idx = entry->frame_index;
			if (frame_idx >= 0 && frame_idx < max_frames) {
				FrameTracker *frame = &frame_table[frame_idx];
				if (frame->busy && frame->pte == entry) {
					replacement_remove(frame_idx);
					if (entry->global_page) tlb_invalidate_vpn(frame->owner_va >> 12);
					else tlb_invalidate_asid(frame->owner_va >> 12, frame->owner_asid);
					physical_free_frame(frame_idx);
					frame->busy = false;
					frame->pte = NULL;
					frame->owner_va = 0;
					frame->owner_asid = 0;
				}
			}
			entry->frame_index = -1;
		} else if (entry->present) {
			// Walk down to child tables before returning this node to the pool
			vmm_destroy_page_table((Table_Node *)entry->address, level - 1);
		}
	}

	free_node(table);
}

static uint8_t vmm_evict_frame(void) {
	if (replacement_head < 0) {
		fprintf(stderr, "[VMM Error] No resident frame is available to evict.\n");
		exit(EXIT_FAILURE);
	}

	uint8_t victim = (uint8_t)replacement_head;
	FrameTracker *frame = &frame_table[victim];
	Page_Entry *pte = frame->pte;
	if (!frame->busy || !pte) {
		fprintf(stderr, "[VMM Error] Replacement queue contains an invalid frame.\n");
		exit(EXIT_FAILURE);
	}

	// Save the oldest resident page in a free or newly allocated swap slot.
	uint64_t swap_offset;
	if (!swap_slot_allocate(&swap_offset)) {
		fprintf(stderr, "[VMM Error] Swap file offset limit reached.\n");
		exit(EXIT_FAILURE);
	}
	if (!swap_seek(swap_offset) ||
		fwrite(physical_memory[victim], 1, page_size, swap_file) != page_size) {
		perror("Failed to write page to swap");
		exit(EXIT_FAILURE);
	}
	pte->swap_offset = swap_offset;
	pte->dirty = false;
	pte->present = false;
	pte->swapped = true;
	pte->frame_index = -1;

	// Remove the old owner and its cached translation before frame reuse
	if (pte->global_page) tlb_invalidate_vpn(frame->owner_va >> 12);
	else tlb_invalidate_asid(frame->owner_va >> 12, frame->owner_asid);
	replacement_remove(victim);
	frame->busy = false;
	frame->pte = NULL;
	frame->owner_va = 0;
	frame->owner_asid = 0;
	return victim;
}

MMU_Result vmm_handle_page_fault(Page_Entry *pte, uint64_t va,
		Access_Type access, bool is_user, uint64_t *frame_base) {
	if (!pte || !pte->mapped || !current_process || !frame_base) {
		return mmu_fault_invalid_argument;
	}

	// Recheck access before allocation so a rejected fault has no side effects.
	if ((access & access_read) && !pte->readable) return mmu_fault_read_permission;
	if ((access & access_write) && !pte->writeable) return mmu_fault_write_permission;
	if ((access & access_exec) && !pte->executable) return mmu_fault_execute_permission;
	if (is_user && !pte->user_mode) return mmu_fault_privilege;

	printf("[VMM Fault Handler] Demand paging VA 0x%012" PRIX64 "...\n", va);
	int frame_idx = physical_frame_alloc();
	if (frame_idx == -1) frame_idx = vmm_evict_frame();

	// Restore a swapped page, or start a new mapping with zero-filled memory
	if (pte->swapped) {
		if (!swap_seek(pte->swap_offset) ||
			fread(physical_memory[frame_idx], 1, page_size, swap_file) != page_size) {
			perror("Failed to read page from swap");
			exit(EXIT_FAILURE);
		}
		swap_slot_release(pte->swap_offset);
		pte->swap_offset = 0;
	} else {
		memset(physical_memory[frame_idx], 0, page_size);
	}

	pte->address = (uint64_t)physical_memory[frame_idx];
	pte->present = true;
	pte->swapped = false;
	pte->frame_index = (int16_t)frame_idx;

	frame_table[frame_idx].busy = true;
	frame_table[frame_idx].pte = pte;
	frame_table[frame_idx].owner_va = va;
	frame_table[frame_idx].owner_asid = current_process->asid;
	replacement_append(frame_idx);

	// Return the frame base; the MMU adds the virtual page offset.
	*frame_base = pte->address;
	return mmu_ok;
}

Page_Entry *vmm_find_global_page(uint64_t vpn) {
	Global_Mapping *entry = find_global_mapping(vpn);
	if (!entry || !entry->owner || !entry->pte || !entry->pte->mapped ||
		!entry->pte->global_page) return NULL;
	return entry->pte;
}
