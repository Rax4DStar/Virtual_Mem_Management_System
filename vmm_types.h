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

#define get_L4(va) (((va)>>39)& 0x1FF) //shift bits to lower end.
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

typedef struct{
    uint64_t address; //store physical mem
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
} TLB_Entry; //define TLB to cache translations 

typedef struct{
    Page_Entry *pte; //point to page entry
    uint64_t owner_va;
    bool busy;
} FrameTracker; //track frame ownership and if occupied or not

typedef struct{
    asid_t asid;
    Table_Node *root; // point to root of process table heirarchy 
} Process;

extern asid_t current_asid; //extern informs compiler variable exists elsewhere

#endif