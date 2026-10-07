#ifndef VMM_H
#define VMM_H

#include "vmm_types.h"

void vmm_init(void); 
void vmm_cleanup(void); 

Table_Node* vmm_create_process_table(void); //create root pagetable
// Select the root and ASID together; duplicate ASIDs or roots among live
// processes are rejected.
bool vmm_switch_process(Process *process);
// Map or replace a virtual page; a frame is allocated on first access. A
// global VA has one canonical owning mapping shared by every process.
bool vmm_mmap(Process *process, uint64_t va, bool writable, bool executable,
              bool user_mode, bool global_page);
// Remove a mapping and release its resident frame, if it has one.
bool vmm_munmap(Process *process, uint64_t va);
// Copy bytes between a translated physical address and simulated memory.
bool vmm_read_physical(uint64_t address, void *buffer, size_t size);
bool vmm_write_physical(uint64_t address, const void *buffer, size_t size);
//forms a map VA-->mmap-->page tables-->page entry-->physical frame. 

void vmm_destroy_page_table(Table_Node *table, int level);
MMU_Result vmm_handle_page_fault(Page_Entry *pte, uint64_t va,
		Access_Type access, bool is_user, uint64_t *frame_base);
// Resolve a globally mapped VPN to its canonical page entry.
Page_Entry *vmm_find_global_page(uint64_t vpn);

#endif
