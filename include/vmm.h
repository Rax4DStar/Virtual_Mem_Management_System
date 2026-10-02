#ifndef VMM_H
#define VMM_H

#include "vmm_types.h"

void vmm_init(void); 
void vmm_cleanup(void); 

Table_Node* vmm_create_process_table(void); //create root pagetable
// Select the root and ASID together; live processes should have unique ASIDs.
bool vmm_switch_process(Process *process);
// Map or replace a virtual page; a frame is allocated on first access.
bool vmm_mmap(Process *process, uint64_t va, bool writable, bool executable,
              bool user_mode, bool global_page);
// Remove a mapping and release its resident frame, if it has one.
bool vmm_munmap(Process *process, uint64_t va);
// Copy bytes between a translated physical address and simulated memory.
bool vmm_read_physical(uint64_t address, void *buffer, size_t size);
bool vmm_write_physical(uint64_t address, const void *buffer, size_t size);
//forms a map VA-->mmap-->page tables-->page entry-->physical frame. 

void vmm_destroy_page_table(Table_Node *table, int level);
uint64_t vmm_handle_page_fault(Table_Node *root, uint64_t va, Access_Type access); 

#endif
