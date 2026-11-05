#ifndef VMM_STATS_INTERNAL_H
#define VMM_STATS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

// Accounting hooks used by the MMU implementation.
void vmm_stats_record_tlb_lookup(bool hit);
void vmm_stats_record_transfer(bool write, size_t bytes);

#endif
