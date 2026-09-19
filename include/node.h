#ifndef Node_Pool_H
#define Node_Pool_H

#include "vmm_types.h"

Table_Node* alloc_node(void); //func that returns to a Table Node
void free_node(Table_Node *node); //func that returns nothing from a pointer

#endif