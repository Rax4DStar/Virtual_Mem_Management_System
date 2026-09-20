#include "vmm_types.h"
#include "node.h"

#define Node_BitMap_Size (max_table_nodes/64) //bitmap to track if Table_Node is used/free

static Table_Node node_pool[max_table_nodes];
static uint64_t bitmap[Node_BitMap_Size] = {0}; // Statically allocated pool of page-table nodes

static void local_memset(void *ptr, int value, size_t num) { // Clear memory func
    unsigned char *p = (unsigned char *)ptr;
    while (num--) {
        *p++ = (unsigned char)value; //*p++ moves us byte by byte till req range cleared
    }
}

Table_Node* pool_alloc_node(void) { //bitmap search func for free node
    for (int i = 0; i < Node_BitMap_Size; i++) {
        if (bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) { //check if all 64 nodes are taken
            for (int bit = 0; bit < 64; bit++) {
                if (!(bitmap[i] & (1ULL << bit))) { //1ULL is set to bin 1 to enable & checking.
                    bitmap[i] |= (1ULL << bit); // OR func to mark a free bit as 1/in use
                    
                    int index = (i * 64) + bit;
                    Table_Node *node = &node_pool[index];//calc node pool index and set our ptr there

                    local_memset(node, 0, sizeof(Table_Node));
                    return node; //return a clean node 
                }
            }
        }
    }
    return NULL;  // Every node slot is already in use
}
void pool_free_node(Table_Node *node) {
    if (!node) return; // no node to free return nothing
    
    ptrdiff_t index = node - node_pool; //locate which node within a given pool
    if (index < 0 || index >= max_table_nodes) return;

    int array_idx = index / 64; //find bitmap 
    int bit_idx = index % 64; //find bit in bitmap
    bitmap[array_idx] &= ~(1ULL << bit_idx); //set bit to free
}