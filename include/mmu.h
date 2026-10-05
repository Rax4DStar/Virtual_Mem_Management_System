#ifndef MMU_H
#define MMU_H

#include "vmm_types.h"

MMU_Result mmu_translate(uint64_t va, Access_Type access_type, bool is_user,
                         uint64_t *physical_address);
// Copy bytes through the active process's virtual address space.
MMU_Result mmu_read(uint64_t va, void *buffer, size_t size, bool is_user);
MMU_Result mmu_write(uint64_t va, const void *buffer, size_t size, bool is_user);
const char *mmu_result_string(MMU_Result result);

/* Translation uses the root and ASID from the currently selected process. */

#endif
