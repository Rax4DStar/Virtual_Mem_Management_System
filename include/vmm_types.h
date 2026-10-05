#ifndef vmm_types_h 
#define vmm_types_h

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define page_size 4096
#define max_entries 512 
#define tlb_size 16
#define max_frames 128
#define max_table_nodes 256

#define get_L4(va) (((va)>>39)& 0x1FF) //shift bits to lower end
#define get_L3(va) (((va)>>30)& 0x1FF)
#define get_L2(va) (((va)>>21)& 0x1FF)
#define get_L1(va) (((va)>>12)& 0x1FF)
#define get_offset(va) ((va)& 0xFFF) //FFF to extract 12 bits instead of 9

typedef uint16_t asid_t; //16 bit address identifier

typedef enum{
    access_read = 1<<0,
    access_write = 1<<1,
    access_exec = 1<<2
} Access_Type; //bit flag logic to produce memory access

typedef enum {
    mmu_ok = 0,
    mmu_fault_unmapped,
    mmu_fault_read_permission,
    mmu_fault_write_permission,
    mmu_fault_execute_permission,
    mmu_fault_privilege,
    mmu_fault_invalid_argument,
    mmu_fault_invalid_physical_address
} MMU_Result;

typedef struct{
    uint64_t address; //store physical mem
    bool mapped; //a virtual page has a mapping, even if it is not resident
    bool present;
    bool readable;
    bool writeable;
    bool executable;
    bool user_mode;
    bool global_page;
    bool accessed;
    bool dirty;
    bool swapped;
    uint32_t swap_offset;
    int16_t frame_index; //resident frame slot, or -1 when not resident
} Page_Entry; //page table entry representation

typedef struct Table_Node{
    Page_Entry entries[max_entries]; //max no. of entries per table
} Table_Node;

typedef struct{
    uint64_t vpn; //virtual
    uint64_t frame_base; //physical
    asid_t asid;
    bool valid;
    bool global_page;
    // Cache the access controls with the translation so TLB hits enforce them too
    bool readable;
    bool writeable;
    bool executable;
    bool user_mode;
    Page_Entry *pte; //page entry to update access flags on a TLB hit
} TLB_Entry; //define TLB to cache translations 

typedef struct{
    Page_Entry *pte; //point to page entry
    uint64_t owner_va;
    asid_t owner_asid; //address space whose translation must be invalidated
    int16_t replacement_prev;
    int16_t replacement_next;
    bool busy;
} FrameTracker; //track frame ownership and if occupied or not

typedef struct{
    asid_t asid; //must identify this address space in the TLB
    Table_Node *root; // point to root of process table heirarchy 
} Process;

extern Process *current_process; //process selected for address translation

#endif
