#ifndef TLB_H
#define TLB_H

#include "vmm_types.h"

void tlb_init(void); 
int64_t tlb_lookup(uint64_t vpn, asid_t asid); //check if we have a transaltion uint64 to return pos and neg values.
void tlb_insert(uint64_t vpn, uint64_t frame_base, asid_t asid, bool global_page); //add translation to the TLB
void tlb_invalidate_asid(uint64_t vpn, asid_t asid); //used to remove or invalidate 

#endif