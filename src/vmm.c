#include "vmm_types.h"
#include "vmm.h"
#include "Frame_Alloc.h"
#include "tlb.h"
#include "node.h"

#include <inttypes.h>

static FrameTracker frame_table[max_frames];
Process *current_process = NULL;
/* Translation combines a frame base with the page offset, so every frame must
 begin at an address whose low page-offset bits are zero */
_Static_assert((page_size & (page_size - 1)) == 0,
	"page_size must be a power of two for aligned frame addressing");
static _Alignas(page_size) uint8_t physical_memory[max_frames][page_size];
static FILE *swap_file = NULL;
// Resident frames form a FIFO queue so victim selection does not scan frames
static int16_t replacement_head = -1;
static int16_t replacement_tail = -1;
static uint32_t next_swap_offset = 0;

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

	// Remove any cached copy before clearing the page entry.
	tlb_invalidate_vpn(va >> 12);
	memset(pte, 0, sizeof(*pte));
	pte->frame_index = -1;
}

void vmm_init(void) {
	// Start with an empty TLB and reset frame ownership
	current_process = NULL;
	tlb_init();
	physical_frame_allocator_init();
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
	current_process = process;
	return true;
}

bool vmm_mmap(Process *process, uint64_t va, bool writable, bool executable,
		bool user_mode, bool global_page) {
	if (!process || !process->root) return false;
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

	// Release a resident frame and clear any cached translation.
	release_page_mapping(va, pte);

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

void vmm_destroy_page_table(Table_Node *table, int level) {
	if (!table) return;

	for (int i = 0; i < max_entries; i++) {
		Page_Entry *entry = &table->entries[i];

		if (level == 1) {
			if (!entry->present) continue;

			// The PTE records its resident frame, avoiding a scan of all frames
			int frame_idx = entry->frame_index;
			if (frame_idx >= 0 && frame_idx < max_frames) {
				FrameTracker *frame = &frame_table[frame_idx];
				if (frame->busy && frame->pte == entry) {
					replacement_remove(frame_idx);
					tlb_invalidate_asid(frame->owner_va >> 12, frame->owner_asid);
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

	// FIFO gives fixed-time victim selection; swap I/O still depends on the file
	if (fseek(swap_file, (long)next_swap_offset, SEEK_SET) != 0 ||
		fwrite(physical_memory[victim], 1, page_size, swap_file) != page_size) {
		perror("Failed to write page to swap");
		exit(EXIT_FAILURE);
	}
	pte->swap_offset = next_swap_offset;
	next_swap_offset += page_size;
	pte->dirty = false;
	pte->present = false;
	pte->swapped = true;
	pte->frame_index = -1;

	// Remove the old owner and its cached translation before frame reuse
	tlb_invalidate_asid(frame->owner_va >> 12, frame->owner_asid);
	replacement_remove(victim);
	frame->busy = false;
	frame->pte = NULL;
	frame->owner_va = 0;
	frame->owner_asid = 0;
	return victim;
}

uint64_t vmm_handle_page_fault(Table_Node *root, uint64_t va, Access_Type access) {
	(void)access;
	printf("[VMM Fault Handler] Demand paging VA 0x%012" PRIX64 "...\n", va);

	int frame_idx = physical_frame_alloc();
	if (frame_idx == -1) {
		frame_idx = vmm_evict_frame();
	}

	// Walk the existing page tables to find this virtual page's entry
	uint16_t l4 = get_L4(va);
	uint16_t l3 = get_L3(va);
	uint16_t l2 = get_L2(va);
	uint16_t l1 = get_L1(va);
	Table_Node *l3_table = (Table_Node *)root->entries[l4].address;
	Table_Node *l2_table = (Table_Node *)l3_table->entries[l3].address;
	Table_Node *l1_table = (Table_Node *)l2_table->entries[l2].address;
	Page_Entry *pte = &l1_table->entries[l1];

	// Restore a swapped page, or start a new mapping with zero-filled memory
	if (pte->swapped) {
		if (fseek(swap_file, (long)pte->swap_offset, SEEK_SET) != 0 ||
			fread(physical_memory[frame_idx], 1, page_size, swap_file) != page_size) {
			perror("Failed to read page from swap");
			exit(EXIT_FAILURE);
		}
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

	// Return the frame base; the MMU adds the virtual page offset
	return pte->address;
}
