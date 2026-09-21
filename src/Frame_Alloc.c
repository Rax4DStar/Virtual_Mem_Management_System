#include "vmm_types.h"
#include "Frame_Alloc.h"

#define frame_map_size (max_frames/64) 
static uint64_t frame_bit_map[frame_map_size]={0};

int physical_frame_alloc(void){ //find free physical frame
    for( int i = 0; i<frame_map_size; i++){
        for (int bit = 0; bit < 64; bit++) {
            if (!(frame_bit_map[i] & (1ULL << bit))) {
                frame_bit_map[i] |= (1ULL << bit);
                return (i * 64) + bit; //return frame index
            }
        }
    }
    return -1;
}

void physical_free_frame(int frame_idx) { // free frame index
    if (frame_idx < 0 || frame_idx >= max_frames) return;
    int array_idx = frame_idx / 64; //find bitmap location
    int bit_idx = frame_idx % 64; //find bit location
    frame_bit_map[array_idx] &= ~(1ULL << bit_idx); //set bit to 0 aka free
}