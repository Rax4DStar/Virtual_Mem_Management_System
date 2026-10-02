#ifndef MMU_H
#define MMU_H

#include "vmm_types.h"

uint64_t mmu_translate(uint64_t va, Access_Type access_type, bool is_user);
// Copy bytes through the active process's virtual address space.
bool mmu_read(uint64_t va, void *buffer, size_t size, bool is_user);
bool mmu_write(uint64_t va, const void *buffer, size_t size, bool is_user);

/* Translation uses the root and ASID from the currently selected process. */

#endif
