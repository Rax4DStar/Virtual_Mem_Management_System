#include "vmm_types.h"
#include "Frame_Alloc.h"

// A stack of available indices makes both allocation and release O(1)
static int free_frames[max_frames];
static bool frame_in_use[max_frames];
static int free_frame_count = 0;

void physical_frame_allocator_init(void) {
    // Fill the stack so every frame starts out free.
    free_frame_count = max_frames;
    for (int i = 0; i < max_frames; i++) {
        free_frames[i] = max_frames - 1 - i;
        frame_in_use[i] = false;
    }
}

int physical_frame_alloc(void) {
    // No free frames remain; the caller can try eviction
    if (free_frame_count == 0) return -1;

    // Take one index from the top of the free stack
    int frame_idx = free_frames[--free_frame_count];
    frame_in_use[frame_idx] = true;
    return frame_idx;
}

void physical_free_frame(int frame_idx) {
    // Ignore invalid or already-free frame numbers
    if (frame_idx < 0 || frame_idx >= max_frames || !frame_in_use[frame_idx]) return;

    // Mark this frame free and put it back on the stack
    frame_in_use[frame_idx] = false;
    free_frames[free_frame_count++] = frame_idx;
}
