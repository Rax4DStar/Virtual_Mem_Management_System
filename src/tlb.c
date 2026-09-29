#include "vmm_types.h"
#include "tlb.h"

static TLB_Entry tlb[tlb_size];
asid_t current_asid = 1;

// Direct mapping avoids searching: all operations inspect only the VPN's slot
static size_t tlb_index(uint64_t vpn) {
    return (size_t)(vpn % tlb_size);
}

void tlb_init(void) { //mark all TBL entries invalid/empty
    for (int i = 0; i < tlb_size; i++) tlb[i].valid = false;
}

bool tlb_lookup(uint64_t vpn, asid_t asid, TLB_Entry *result) {
    TLB_Entry *entry = &tlb[tlb_index(vpn)];
    if (!entry->valid || entry->vpn != vpn ||
        (!entry->global_page && entry->asid != asid)) {
        return false;
    }

    if (result) *result = *entry;
    return true;
}

void tlb_insert(uint64_t vpn, const Page_Entry *pte, asid_t asid) {
    TLB_Entry *entry = &tlb[tlb_index(vpn)];
    *entry = (TLB_Entry){
        .vpn = vpn,
        .frame_base = pte->address,
        .asid = asid,
        .valid = true,
        .global_page = pte->global_page,
        .readable = pte->readable,
        .writeable = pte->writeable,
        .executable = pte->executable,
        .user_mode = pte->user_mode
    };
}

void tlb_invalidate_asid(uint64_t vpn, asid_t asid) {
    TLB_Entry *entry = &tlb[tlb_index(vpn)];
    if (entry->valid && entry->vpn == vpn && entry->asid == asid) {
        entry->valid = false;
    }
}
