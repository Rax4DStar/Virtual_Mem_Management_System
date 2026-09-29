#ifndef Physical_Frame_Allocator_H
#define Physical_Frame_Allocator_H

void physical_frame_allocator_init(void); // reset the O(1) free-frame stack
int physical_frame_alloc(void); // allocate a frame
void physical_free_frame(int frame_idx); //inform frame can be reused or is out of use

#endif
