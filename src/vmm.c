#include "vmm_types.h"
#include "vmm.h"
#include "Frame_Alloc.h"
#include "tlb.h"
#include "node.h"
#include "vmm_stats_internal.h"

#include <inttypes.h>
#include <limits.h>

static FrameTracker frame_table[max_frames];
Process *current_process = NULL;
static VMM_Stats statistics;

typedef struct {
	Process *process;
	Table_Node *root;
	asid_t asid;
} Process_Registration;

typedef struct {
	uintptr_t key;
	uint16_t registry_slot;
	uint8_t state; // 0 empty, 1 occupied, 2 deleted
} Registration_Index_Entry;

typedef struct {
	uint64_t vpn;
	Process *owner;
	Page_Entry *pte;
	uint8_t state; //0 empty, 1 occupied, 2 deleted
} Global_Mapping;

typedef struct Shared_Page {
	Page_Entry *members;
	uint32_t references;
	uint64_t address;
	uint64_t swap_offset;
	int16_t frame_index;
	bool present;
	bool swapped;
	bool writable;
	bool accessed;
} Shared_Page;

#define global_mapping_capacity (max_table_nodes * max_entries)
#define registration_index_capacity (max_table_nodes * 2)
#define max_shared_pages (max_table_nodes * max_entries)
static Process_Registration process_registry[max_table_nodes];
static uint16_t process_by_asid[UINT16_MAX + 1u];
static Registration_Index_Entry process_by_pointer[registration_index_capacity];
static Registration_Index_Entry process_by_root[registration_index_capacity];
static uint16_t free_process_slots[max_table_nodes];
static size_t free_process_count = 0;
static Global_Mapping global_mappings[global_mapping_capacity];
static Shared_Page shared_pages[max_shared_pages];
static uint32_t free_shared_slots[max_shared_pages];
static size_t free_shared_count = 0;
/* Translation combines a frame base with the page offset, so every frame must
 begin at an address whose low page-offset bits are zero */
_Static_assert((page_size & (page_size - 1)) == 0,
	"page_size must be a power of two for aligned frame addressing");
static _Alignas(page_size) uint8_t physical_memory[max_frames][page_size];
static FILE *swap_file = NULL;
// Resident frames form a FIFO queue so victim selection does not scan frames
static int16_t replacement_head = -1;
static int16_t replacement_tail = -1;
static int16_t replacement_clock_hand = -1;
static VMM_Replacement_Policy replacement_policy = vmm_replacement_fifo;
// A mapped page can own at most one swap slot.
static uint64_t free_swap_offsets[max_table_nodes * max_entries];
static size_t free_swap_count = 0;
static uint64_t next_swap_offset = 0;

VMM_Stats vmm_get_stats(void) {
	return statistics;
}

void vmm_reset_stats(void) {
	memset(&statistics, 0, sizeof(statistics));
}

bool vmm_set_replacement_policy(VMM_Replacement_Policy policy) {
	if (policy != vmm_replacement_fifo && policy != vmm_replacement_clock) {
		return false;
	}
	replacement_policy = policy;
	if (replacement_policy == vmm_replacement_clock && replacement_clock_hand < 0) {
		replacement_clock_hand = replacement_head;
	}
	return true;
}

VMM_Replacement_Policy vmm_get_replacement_policy(void) {
	return replacement_policy;
}

void vmm_stats_record_tlb_lookup(bool hit) {
	if (hit) statistics.tlb_hits++;
	else statistics.tlb_misses++;
}

void vmm_stats_record_transfer(bool write, size_t bytes) {
	if (write) statistics.bytes_written += bytes;
	else statistics.bytes_read += bytes;
}

_Static_assert((registration_index_capacity & (registration_index_capacity - 1)) == 0,
	"registration index capacity must be a power of two");

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

static size_t registration_hash(uintptr_t key) {
	key ^= key >> 17;
	key *= (uintptr_t)0xed5ad4bbU;
	key ^= key >> 11;
	key *= (uintptr_t)0xac4c1b51U;
	key ^= key >> 15;
	return (size_t)key & (registration_index_capacity - 1);
}

static Registration_Index_Entry *registration_index_find(
		Registration_Index_Entry *index, uintptr_t key) {
	size_t slot = registration_hash(key);
	for (size_t probe = 0; probe < registration_index_capacity; probe++) {
		Registration_Index_Entry *entry = &index[slot];
		if (entry->state == 0) return NULL;
		if (entry->state == 1 && entry->key == key) return entry;
		slot = (slot + 1) & (registration_index_capacity - 1);
	}
	return NULL;
}

static bool registration_index_insert(Registration_Index_Entry *index,
		uintptr_t key, uint16_t registry_slot) {
	size_t slot = registration_hash(key);
	size_t first_deleted = registration_index_capacity;
	for (size_t probe = 0; probe < registration_index_capacity; probe++) {
		Registration_Index_Entry *entry = &index[slot];
		if (entry->state == 1 && entry->key == key) return false;
		if (entry->state == 2 && first_deleted == registration_index_capacity) {
			first_deleted = slot;
		}
		if (entry->state == 0) {
			if (first_deleted != registration_index_capacity) slot = first_deleted;
			index[slot] = (Registration_Index_Entry){
				.key = key,
				.registry_slot = registry_slot,
				.state = 1
			};
			return true;
		}
		slot = (slot + 1) & (registration_index_capacity - 1);
	}
	if (first_deleted == registration_index_capacity) return false;
	index[first_deleted] = (Registration_Index_Entry){
		.key = key,
		.registry_slot = registry_slot,
		.state = 1
	};
	return true;
}

static void registration_index_remove(Registration_Index_Entry *entry) {
	if (entry) entry->state = 2;
}

static bool register_process(Process *process) {
	uint16_t asid_slot = process_by_asid[process->asid];
	if (asid_slot != 0) {
		Process_Registration *entry = &process_registry[asid_slot - 1];
		return entry->process == process && entry->root == process->root &&
			entry->asid == process->asid;
	}

	Registration_Index_Entry *pointer_entry = registration_index_find(
		process_by_pointer, (uintptr_t)process);
	if (pointer_entry) return false; // A live process cannot change its ASID.
	if (registration_index_find(process_by_root, (uintptr_t)process->root)) return false;
	if (free_process_count == 0) return false;

	uint16_t registry_slot = free_process_slots[--free_process_count];
	process_registry[registry_slot] = (Process_Registration){
		.process = process,
		.root = process->root,
		.asid = process->asid
	};
	process_by_asid[process->asid] = registry_slot + 1;
	// Capacity is twice the maximum live process count, so each index has room.
	if (!registration_index_insert(process_by_pointer, (uintptr_t)process,
			registry_slot)) {
		process_by_asid[process->asid] = 0;
		memset(&process_registry[registry_slot], 0, sizeof(process_registry[registry_slot]));
		free_process_slots[free_process_count++] = registry_slot;
		return false;
	}
	if (!registration_index_insert(process_by_root, (uintptr_t)process->root,
			registry_slot)) {
		registration_index_remove(registration_index_find(process_by_pointer,
			(uintptr_t)process));
		process_by_asid[process->asid] = 0;
		memset(&process_registry[registry_slot], 0, sizeof(process_registry[registry_slot]));
		free_process_slots[free_process_count++] = registry_slot;
		return false;
	}
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
	Registration_Index_Entry *root_entry = registration_index_find(
		process_by_root, (uintptr_t)root);
	if (root_entry) {
		uint16_t registry_slot = root_entry->registry_slot;
		Process_Registration *entry = &process_registry[registry_slot];
		Process *process = entry->process;
		if (process && current_process == process) current_process = NULL;
		if (process) {
			process_by_asid[entry->asid] = 0;
			registration_index_remove(registration_index_find(process_by_pointer,
				(uintptr_t)process));
		}
		registration_index_remove(root_entry);
		memset(entry, 0, sizeof(*entry));
		free_process_slots[free_process_count++] = registry_slot;
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
	if (replacement_clock_hand == frame_idx) {
		int16_t next = frame->replacement_next >= 0
			? frame->replacement_next : replacement_head;
		replacement_clock_hand = next == frame_idx ? -1 : next;
	}
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
	if (replacement_clock_hand < 0) replacement_clock_hand = (int16_t)frame_idx;
}

static Shared_Page *shared_page_allocate(void) {
	if (free_shared_count == 0) return NULL;
	uint32_t slot = free_shared_slots[--free_shared_count];
	Shared_Page *shared = &shared_pages[slot];
	memset(shared, 0, sizeof(*shared));
	shared->frame_index = -1;
	return shared;
}

static void shared_page_release(Shared_Page *shared) {
	ptrdiff_t slot = shared - shared_pages;
	memset(shared, 0, sizeof(*shared));
	free_shared_slots[free_shared_count++] = (uint32_t)slot;
}

static void shared_member_add(Shared_Page *shared, Page_Entry *pte) {
	pte->shared_page = shared;
	pte->shared_prev = NULL;
	pte->shared_next = shared->members;
	if (shared->members) shared->members->shared_prev = pte;
	shared->members = pte;
	shared->references++;
}

static void shared_member_remove(Shared_Page *shared, Page_Entry *pte) {
	if (pte->shared_prev) pte->shared_prev->shared_next = pte->shared_next;
	else shared->members = pte->shared_next;
	if (pte->shared_next) pte->shared_next->shared_prev = pte->shared_prev;
	pte->shared_page = NULL;
	pte->shared_prev = NULL;
	pte->shared_next = NULL;
	shared->references--;
}

static void shared_sync_members(Shared_Page *shared) {
	for (Page_Entry *pte = shared->members; pte; pte = pte->shared_next) {
		pte->address = shared->address;
		pte->present = shared->present;
		pte->swapped = shared->swapped;
		pte->swap_offset = shared->swap_offset;
		pte->frame_index = shared->frame_index;
		pte->accessed = shared->accessed;
	}
}

static void shared_free_backing(Shared_Page *shared) {
	if (shared->present && shared->frame_index >= 0 &&
		shared->frame_index < max_frames) {
		int frame_idx = shared->frame_index;
		FrameTracker *frame = &frame_table[frame_idx];
		if (frame->busy && frame->shared_page == shared) {
			replacement_remove(frame_idx);
			physical_free_frame(frame_idx);
			frame->busy = false;
			frame->pte = NULL;
			frame->shared_page = NULL;
			frame->owner_va = 0;
			frame->owner_asid = 0;
		}
	}
	if (shared->swapped) swap_slot_release(shared->swap_offset);
}

static void shared_note_access(Page_Entry *pte) {
	pte->accessed = true;
	if (pte->shared_page) pte->shared_page->accessed = true;
}

void vmm_note_page_access(Page_Entry *pte) {
	shared_note_access(pte);
}

static int16_t replacement_select_victim(void) {
	if (replacement_policy == vmm_replacement_fifo) return replacement_head;
	if (replacement_clock_hand < 0) replacement_clock_hand = replacement_head;
	if (replacement_clock_hand < 0) return -1;

	// Give referenced pages a second chance, then choose the first clear page.
	for (;;) {
		int16_t candidate = replacement_clock_hand;
		FrameTracker *frame = &frame_table[candidate];
		if (!frame->busy || (!frame->pte && !frame->shared_page)) return -1;
		statistics.clock_page_checks++;
		replacement_clock_hand = frame->replacement_next >= 0
			? frame->replacement_next : replacement_head;
		bool accessed = frame->shared_page
			? frame->shared_page->accessed : frame->pte->accessed;
		if (!accessed) return candidate;
		if (frame->shared_page) {
			frame->shared_page->accessed = false;
		} else {
			frame->pte->accessed = false;
		}
	}
}

static void release_page_mapping(uint64_t va, Page_Entry *pte) {
	if (pte->shared_page) {
		Shared_Page *shared = pte->shared_page;
		tlb_invalidate_vpn(pte->vpn);
		shared_member_remove(shared, pte);
		if (shared->references == 0) {
			shared_free_backing(shared);
			shared_page_release(shared);
		} else if (shared->references == 1) {
			Page_Entry *remaining = shared->members;
			remaining->copy_on_write = false;
			remaining->writeable = shared->writable;
			remaining->accessed = shared->accessed;
			remaining->shared_page = NULL;
			remaining->shared_prev = NULL;
			remaining->shared_next = NULL;
			if (shared->present && shared->frame_index >= 0 &&
				shared->frame_index < max_frames) {
				FrameTracker *frame = &frame_table[shared->frame_index];
				if (frame->busy && frame->shared_page == shared) {
					frame->shared_page = NULL;
					frame->pte = remaining;
					frame->owner_va = remaining->vpn << 12;
					frame->owner_asid = remaining->owner_asid;
				}
			}
			tlb_invalidate_vpn(remaining->vpn);
			shared->members = NULL;
			shared->references = 0;
			shared_page_release(shared);
		}
		memset(pte, 0, sizeof(*pte));
		pte->frame_index = -1;
		return;
	}

	if (pte->present && pte->frame_index >= 0 && pte->frame_index < max_frames) {
		int frame_idx = pte->frame_index;
		FrameTracker *frame = &frame_table[frame_idx];
		if (frame->busy && frame->pte == pte) {
			replacement_remove(frame_idx);
			physical_free_frame(frame_idx);
			frame->busy = false;
			frame->pte = NULL;
			frame->shared_page = NULL;
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
	vmm_reset_stats();
	memset(process_registry, 0, sizeof(process_registry));
	memset(process_by_asid, 0, sizeof(process_by_asid));
	memset(process_by_pointer, 0, sizeof(process_by_pointer));
	memset(process_by_root, 0, sizeof(process_by_root));
	for (int i = 0; i < max_table_nodes; i++) {
		free_process_slots[i] = (uint16_t)(max_table_nodes - 1 - i);
	}
	free_process_count = max_table_nodes;
	memset(global_mappings, 0, sizeof(global_mappings));
	memset(shared_pages, 0, sizeof(shared_pages));
	for (uint32_t i = 0; i < max_shared_pages; i++) {
		free_shared_slots[i] = (uint32_t)(max_shared_pages - 1 - i);
	}
	free_shared_count = max_shared_pages;
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
		frame_table[i].shared_page = NULL;
		frame_table[i].owner_va = 0;
		frame_table[i].owner_asid = 0;
		frame_table[i].replacement_prev = -1;
		frame_table[i].replacement_next = -1;
	}
	replacement_head = -1;
	replacement_tail = -1;
	replacement_clock_hand = -1;
	replacement_policy = vmm_replacement_fifo;
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
	if (current_process != process) statistics.process_switches++;
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
		root->used_entries++;
	}
	Table_Node *l3_table = (Table_Node *)root->entries[l4].address;

	if (!l3_table->entries[l3].present) {
		l3_table->entries[l3].address = (uint64_t)vmm_create_process_table();
		l3_table->entries[l3].present = true;
		l3_table->used_entries++;
	}
	Table_Node *l2_table = (Table_Node *)l3_table->entries[l3].address;

	if (!l2_table->entries[l2].present) {
		l2_table->entries[l2].address = (uint64_t)vmm_create_process_table();
		l2_table->entries[l2].present = true;
		l2_table->used_entries++;
	}
	Table_Node *l1_table = (Table_Node *)l2_table->entries[l2].address;

	Page_Entry *pte = &l1_table->entries[l1];
	if (global_page && !add_global_mapping(vpn, process, pte)) return false;
	if (!pte->mapped) l1_table->used_entries++;
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
	pte->copy_on_write = false;
	pte->accessed = false;
	pte->dirty = false;
	pte->swapped = false;
	pte->swap_offset = 0;
	pte->vpn = vpn;
	pte->owner_asid = process->asid;
	statistics.mappings++;
	return true;
}

bool vmm_mmap_shared(Process *source, uint64_t source_va,
		Process *target, uint64_t target_va) {
	if (!source || !source->root || !target || !target->root ||
		!register_process(source) || !register_process(target)) return false;
	Page_Entry *source_pte = find_page_entry(source->root, source_va);
	if (!source_pte || !source_pte->mapped || source_pte->global_page) return false;
	if (source == target && source_va == target_va) return false;
	Page_Entry *target_pte = find_page_entry(target->root, target_va);
	if (target_pte && target_pte->mapped) return false;

	Shared_Page *shared = source_pte->shared_page;
	bool new_group = shared == NULL;
	if (new_group) {
		if (source_pte->present &&
			(source_pte->frame_index < 0 || source_pte->frame_index >= max_frames ||
			 !frame_table[source_pte->frame_index].busy ||
			 frame_table[source_pte->frame_index].pte != source_pte)) return false;
		shared = shared_page_allocate();
		if (!shared) return false;
		shared->address = source_pte->address;
		shared->swap_offset = source_pte->swap_offset;
		shared->frame_index = source_pte->frame_index;
		shared->present = source_pte->present;
		shared->swapped = source_pte->swapped;
		shared->writable = source_pte->writeable || source_pte->copy_on_write;
		shared->accessed = source_pte->accessed;
	}

	if (!vmm_mmap(target, target_va, shared->writable,
			source_pte->executable, source_pte->user_mode, false)) {
		if (new_group) shared_page_release(shared);
		return false;
	}
	target_pte = find_page_entry(target->root, target_va);
	if (!target_pte || !target_pte->mapped) {
		if (new_group) shared_page_release(shared);
		return false;
	}

	if (new_group) {
		shared_member_add(shared, source_pte);
		if (shared->present) {
			FrameTracker *frame = &frame_table[shared->frame_index];
			frame->pte = NULL;
			frame->shared_page = shared;
		}
	}
	shared_member_add(shared, target_pte);
	shared_sync_members(shared);
	source_pte->writeable = false;
	source_pte->copy_on_write = shared->writable;
	target_pte->writeable = false;
	target_pte->copy_on_write = shared->writable;
	tlb_invalidate_vpn(source_pte->vpn);
	tlb_invalidate_vpn(target_pte->vpn);
	return true;
}

static bool table_is_empty(Table_Node *table) {
	return table->used_entries == 0;
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
	l1_table->used_entries--;

	// Return page-table nodes that no longer contain mappings.
	if (table_is_empty(l1_table)) {
		free_node(l1_table);
		memset(&l2_table->entries[l2], 0, sizeof(l2_table->entries[l2]));
		l2_table->used_entries--;
	}
	if (table_is_empty(l2_table)) {
		free_node(l2_table);
		memset(&l3_table->entries[l3], 0, sizeof(l3_table->entries[l3]));
		l3_table->used_entries--;
	}
	if (table_is_empty(l3_table)) {
		free_node(l3_table);
		memset(&root->entries[l4], 0, sizeof(root->entries[l4]));
		root->used_entries--;
	}
	statistics.unmaps++;
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
			if (entry->mapped) release_page_mapping(entry->vpn << 12, entry);
		} else if (entry->present) {
			// Walk down to child tables before returning this node to the pool
			vmm_destroy_page_table((Table_Node *)entry->address, level - 1);
		}
	}

	free_node(table);
}

static uint8_t vmm_evict_frame(void) {
	int16_t victim_idx = replacement_select_victim();
	if (victim_idx < 0) {
		fprintf(stderr, "[VMM Error] No resident frame is available to evict.\n");
		exit(EXIT_FAILURE);
	}

	uint8_t victim = (uint8_t)victim_idx;
	FrameTracker *frame = &frame_table[victim];
	Page_Entry *pte = frame->pte;
	Shared_Page *shared = frame->shared_page;
	if (!frame->busy || (!pte && !shared)) {
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
	statistics.page_evictions++;
	statistics.swap_page_outs++;
	if (shared) {
		shared->address = 0;
		shared->swap_offset = swap_offset;
		shared->frame_index = -1;
		shared->present = false;
		shared->swapped = true;
		shared->accessed = false;
		shared_sync_members(shared);
		for (Page_Entry *member = shared->members; member;
			member = member->shared_next) {
			tlb_invalidate_vpn(member->vpn);
		}
	} else {
		pte->swap_offset = swap_offset;
		pte->dirty = false;
		pte->present = false;
		pte->swapped = true;
		pte->frame_index = -1;
		pte->address = 0;
		tlb_invalidate_vpn(pte->vpn);
	}
	replacement_remove(victim);
	frame->busy = false;
	frame->pte = NULL;
	frame->shared_page = NULL;
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

#ifndef VMM_QUIET
	printf("[VMM Fault Handler] Demand paging VA 0x%012" PRIX64 "...\n", va);
#endif
	int frame_idx = physical_frame_alloc();
	if (frame_idx == -1) frame_idx = vmm_evict_frame();

	// Restore a swapped page, or start a new mapping with zero-filled memory
	Shared_Page *shared = pte->shared_page;
	bool swapped_in = shared ? shared->swapped : pte->swapped;
	uint64_t swap_offset = shared ? shared->swap_offset : pte->swap_offset;
	if (swapped_in) {
		if (!swap_seek(swap_offset) ||
			fread(physical_memory[frame_idx], 1, page_size, swap_file) != page_size) {
			perror("Failed to read page from swap");
			exit(EXIT_FAILURE);
		}
		swap_slot_release(swap_offset);
	} else {
		memset(physical_memory[frame_idx], 0, page_size);
	}
	statistics.page_faults++;
	if (swapped_in) statistics.swap_page_ins++;
	else statistics.zero_fill_page_ins++;

	if (shared) {
		shared->address = (uint64_t)physical_memory[frame_idx];
		shared->swap_offset = 0;
		shared->frame_index = (int16_t)frame_idx;
		shared->present = true;
		shared->swapped = false;
		shared->accessed = true;
		shared_sync_members(shared);
	} else {
		pte->address = (uint64_t)physical_memory[frame_idx];
		pte->present = true;
		pte->swapped = false;
		pte->swap_offset = 0;
		pte->frame_index = (int16_t)frame_idx;
		pte->accessed = true;
	}

	frame_table[frame_idx].busy = true;
	frame_table[frame_idx].pte = shared ? NULL : pte;
	frame_table[frame_idx].shared_page = shared;
	frame_table[frame_idx].owner_va = va;
	frame_table[frame_idx].owner_asid = current_process->asid;
	replacement_append(frame_idx);

	// Return the frame base; the MMU adds the virtual page offset.
	*frame_base = shared ? shared->address : pte->address;
	return mmu_ok;
}

MMU_Result vmm_resolve_copy_on_write(Page_Entry *pte, Access_Type access,
		bool is_user) {
	if (!pte || !pte->mapped || !pte->shared_page) return mmu_fault_invalid_argument;
	if (is_user && !pte->user_mode) return mmu_fault_privilege;
	if ((access & access_read) && !pte->readable) return mmu_fault_read_permission;
	if ((access & access_exec) && !pte->executable) return mmu_fault_execute_permission;
	if (!(access & access_write) || !pte->copy_on_write) {
		return mmu_fault_write_permission;
	}

	Shared_Page *shared = pte->shared_page;
	if (!shared->writable) return mmu_fault_write_permission;
	if (shared->references == 1) {
		shared_member_remove(shared, pte);
		pte->writeable = true;
		pte->copy_on_write = false;
		pte->accessed = shared->accessed;
		if (shared->present && shared->frame_index >= 0 &&
			shared->frame_index < max_frames) {
			FrameTracker *frame = &frame_table[shared->frame_index];
			if (frame->busy && frame->shared_page == shared) {
				frame->shared_page = NULL;
				frame->pte = pte;
				frame->owner_va = pte->vpn << 12;
				frame->owner_asid = pte->owner_asid;
			}
		}
		shared_page_release(shared);
		tlb_invalidate_vpn(pte->vpn);
		statistics.copy_on_write_faults++;
		return mmu_ok;
	}
	if (shared->references < 2) return mmu_fault_invalid_argument;

	if (!pte->present) {
		uint64_t frame_base;
		MMU_Result result = vmm_handle_page_fault(pte, pte->vpn << 12,
			access_read, is_user, &frame_base);
		if (result != mmu_ok) return result;
	}

	// Preserve the source bytes even if allocating the private frame evicts it.
	uint8_t page_copy[page_size];
	memcpy(page_copy, (void *)(uintptr_t)pte->address, page_size);
	int frame_idx = physical_frame_alloc();
	if (frame_idx == -1) frame_idx = vmm_evict_frame();
	bool writable = shared->writable;
	shared_member_remove(shared, pte);
	memcpy(physical_memory[frame_idx], page_copy, page_size);

	pte->shared_page = NULL;
	pte->shared_prev = NULL;
	pte->shared_next = NULL;
	pte->address = (uint64_t)physical_memory[frame_idx];
	pte->present = true;
	pte->swapped = false;
	pte->swap_offset = 0;
	pte->frame_index = (int16_t)frame_idx;
	pte->writeable = writable;
	pte->copy_on_write = false;
	pte->accessed = true;
	pte->dirty = (access & access_write) != 0;
	frame_table[frame_idx].busy = true;
	frame_table[frame_idx].pte = pte;
	frame_table[frame_idx].shared_page = NULL;
	frame_table[frame_idx].owner_va = pte->vpn << 12;
	frame_table[frame_idx].owner_asid = pte->owner_asid;
	replacement_append(frame_idx);
	tlb_invalidate_vpn(pte->vpn);
	statistics.copy_on_write_faults++;
	return mmu_ok;
}

Page_Entry *vmm_find_global_page(uint64_t vpn) {
	Global_Mapping *entry = find_global_mapping(vpn);
	if (!entry || !entry->owner || !entry->pte || !entry->pte->mapped ||
		!entry->pte->global_page) return NULL;
	return entry->pte;
}
