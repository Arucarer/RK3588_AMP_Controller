SDK ?= $(HOME)/rk3588_linux_sdk

CROSS_COMPILE := $(SDK)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

BUILD_DIR := build

CFLAGS := \
	-march=armv8-a \
	-mgeneral-regs-only \
	-ffreestanding \
	-fno-builtin \
	-fno-stack-protector \
	-fno-pic \
	-ffixed-x18 \
	-O2 \
	-Wall \
	-Wextra

LDFLAGS := \
	-nostdlib \
	-nostartfiles \
	-Wl,-T,board/rk3588/linker.ld \
	-Wl,-Map,$(BUILD_DIR)/amp_bootstrap.map \
	-Wl,--build-id=none

OBJS := \
	$(BUILD_DIR)/startup.o \
	$(BUILD_DIR)/vector.o \
	$(BUILD_DIR)/main.o \
	$(BUILD_DIR)/irq_timer.o

all: $(BUILD_DIR)/amp_bootstrap.bin

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/startup.o: board/rk3588/startup.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vector.o: board/rk3588/vector.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/main.o: rtos/main.c rtos/irq_timer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/irq_timer.o: rtos/irq_timer.c rtos/irq_timer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/amp_bootstrap.elf: $(OBJS) board/rk3588/linker.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/amp_bootstrap.bin: $(BUILD_DIR)/amp_bootstrap.elf
	$(OBJCOPY) -O binary $< $@
	$(OBJDUMP) -d $< > $(BUILD_DIR)/amp_bootstrap.dis

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean