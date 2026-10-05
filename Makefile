CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude
BUILD_DIR ?= build

VMM_SOURCES = src/Frame_Alloc.c src/mmu.c src/node.c src/tlb.c src/vmm.c

.PHONY: all run test clean

all: $(BUILD_DIR)/vmm_demo

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/vmm_demo: $(VMM_SOURCES) src/main.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VMM_SOURCES) src/main.c -o $@

$(BUILD_DIR)/test_vmm: $(VMM_SOURCES) tests/test_vmm.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VMM_SOURCES) tests/test_vmm.c -o $@

run: $(BUILD_DIR)/vmm_demo
	cd $(BUILD_DIR) && ./vmm_demo

test: $(BUILD_DIR)/test_vmm
	cd $(BUILD_DIR) && ./test_vmm

clean:
	rm -rf $(BUILD_DIR)
