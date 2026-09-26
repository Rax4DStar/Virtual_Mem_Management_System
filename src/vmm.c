#include "vmm_types.h"
#include "vmm.h"
#include "Frame_Alloc.h"
#include "tlb.h"
#include "node.h"

#include <inttypes.h>

static FrameTracker frame_table[max_frames];
static uint8_t physical_memory[max_frames][page_size];
static FILE *swap_file = NULL;
static uint16_t clock_hand = 0;
static uint32_t next_swap_offset = 0;

void vmm_init(void) {
	// Start with an empty TLB and reset frame ownership
	tlb_init();
	swap_file = fopen("swap.bin", "w+b");
	if (!swap_file) {
		perror("Failed to open swap.bin");
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < max_frames; i++) {
		frame_table[i].busy = false;
		frame_table[i].pte = NULL;
		frame_table[i].owner_va = 0;
	}
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

void vmm_mmap(Table_Node *root, uint64_t va, bool writable, bool user_mode, bool global_page) {
	// Create each level so VA reaches a leaf entry.
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
	// Record permissions now.Physical page is allocated on first access
	pte->address = 0;
	pte->present = false;
	pte->readable = true;
	pte->writeable = writable;
	pte->executable = false;
	pte->user_mode = user_mode;
	pte->global_page = global_page;
	pte->accessed = false;
	pte->dirty = false;
	pte->swapped = false;
	pte->swap_offset = 0;
}

void vmm_destroy_page_table(Table_Node *table, int level) {
	if (!table) return;

	for (int i = 0; i < max_entries; i++) {
		Page_Entry *entry = &table->entries[i];

		if (level == 1) {
			if (!entry->present) continue;

			// Only resident(not swapped) pages own frames
			for (int frame_idx = 0; frame_idx < max_frames; frame_idx++) {
				FrameTracker *frame = &frame_table[frame_idx];
				if (frame->busy && frame->pte == entry) {
					tlb_invalidate_asid(frame->owner_va >> 12, current_asid);
					physical_free_frame(frame_idx);
					frame->busy = false;
					frame->pte = NULL;
					frame->owner_va = 0;
					break;
				}
			}
		} else if (entry->present) {
			// Walk down to child tables before returning this node to the pool
			vmm_destroy_page_table((Table_Node *)entry->address, level - 1);
		}
	}

	free_node(table);
}

static uint8_t vmm_evict_frame(void) {
	while (true) {
		FrameTracker *frame = &frame_table[clock_hand];
		Page_Entry *pte = frame->pte;

		if (frame->busy && pte) {
			// Give recently accessed pages a second chance via clock policy
			if (pte->accessed) {
				pte->accessed = false;
			} else {
				uint8_t victim = (uint8_t)clock_hand;

				// Save the page before reusing its physical frame
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

				// Drop the cached translation so cant point to reused frame
				tlb_invalidate_asid(frame->owner_va >> 12, current_asid);
				clock_hand = (clock_hand + 1) % max_frames;
				return victim;
			}
		}

		clock_hand = (clock_hand + 1) % max_frames;
	}
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

	frame_table[frame_idx].busy = true;
	frame_table[frame_idx].pte = pte;
	frame_table[frame_idx].owner_va = va;

	// Return the frame base; the MMU adds the virtual page offset
	return pte->address;
}
