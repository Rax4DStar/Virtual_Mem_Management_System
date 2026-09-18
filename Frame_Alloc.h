#ifndef Physical_Frame_Allocator_H
#define Physical_Frame_Allocator_H

int physical_frame_alloc(void); // allocate a frame
void physical_free_frame(int frame_idx); //inform frame can be reused or is out of use

#endif