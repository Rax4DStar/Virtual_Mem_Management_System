#ifndef MMU_H
#define MMU_H

#include "vmm_types.h"

uint64_t mmu_translate(Table_Node *l4_root, uint64_t va, Access_Type access_type, bool is_user);

/* pointer l4_root points to our Tables root aka Level 4, MMU also checks access type and whether 
a request is being made from the user space or not. VA acts as the adddress we are attempting to translate */

#endif