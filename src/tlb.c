#include "vmm_types.h"
#include "tlb.h"

static TLB_Entry tlb[tlb_size];
static uint8_t tlb_fifo_index = 0; //find next TBL beginning 
asid_t current_asid = 1;

void tlb_init(void) { //mark all TBL entries invalid/empty
    for (int i = 0; i < tlb_size; i++) tlb[i].valid = false;
}

bool tlb_lookup(uint64_t vpn, asid_t asid, TLB_Entry *result) { //find translation and cached access controls
    for (int i = 0; i < tlb_size; i++) {
        if (tlb[i].valid && tlb[i].vpn == vpn) { 
            if (tlb[i].global_page || tlb[i].asid == asid) { //identify if table is normal or global
                if (result) *result = tlb[i];
                return true; //TLB hit
            }
        }
    }
    return false; //TLB miss
}

void tlb_insert(uint64_t vpn, const Page_Entry *pte, asid_t asid) { //new translation
    tlb[tlb_fifo_index].vpn = vpn; //tlb_fifo_index is a place holder 
    tlb[tlb_fifo_index].frame_base = pte->address;
    tlb[tlb_fifo_index].asid = asid;
    tlb[tlb_fifo_index].global_page = pte->global_page;
    tlb[tlb_fifo_index].readable = pte->readable;
    tlb[tlb_fifo_index].writeable = pte->writeable;
    tlb[tlb_fifo_index].executable = pte->executable;
    tlb[tlb_fifo_index].user_mode = pte->user_mode;
    tlb[tlb_fifo_index].valid = true;

    tlb_fifo_index = (tlb_fifo_index + 1) % tlb_size; //wraps around so when TBL full new entry replace oldest
}

void tlb_invalidate_asid(uint64_t vpn, asid_t asid) { //search func to find combination & marks as invalid
    for (int i = 0; i < tlb_size; i++) {
        if (tlb[i].valid && tlb[i].vpn == vpn && tlb[i].asid == asid) {
            tlb[i].valid = false;
        }
    }
}
