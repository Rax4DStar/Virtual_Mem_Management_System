#ifndef VMM_H
#define VMM_H

#include "vmm_types.h"

void vmm_init(void); 
void vmm_cleanup(void); 

Table_Node* vmm_create_process_table(void); //create root pagetable
void vmm_mmap(Table_Node *root, uint64_t va, bool writable, bool user_mode, bool global_page); 
//forms a map VA-->mmap-->page tables-->page entry-->physical frame. 

void vmm_destroy_page_table(Table_Node *table, int level);
uint64_t vmm_handle_page_fault(Table_Node *root, uint64_t va, Access_Type access); 

#endif