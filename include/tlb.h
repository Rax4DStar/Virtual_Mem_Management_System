#ifndef TLB_H
#define TLB_H

#include "vmm_types.h"

void tlb_init(void); 
bool tlb_lookup(uint64_t vpn, asid_t asid, TLB_Entry *result); //copy a matching translation and its permissions
void tlb_insert(uint64_t vpn, const Page_Entry *pte, asid_t asid); //cache translation and access controls
void tlb_invalidate_asid(uint64_t vpn, asid_t asid); //used to remove or invalidate 
void tlb_invalidate_vpn(uint64_t vpn); //remove the cached entry for this virtual page

#endif
