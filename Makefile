SDK ?= $(HOME)/rk3588_linux_sdk

CROSS_COMPILE := $(SDK)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
MKIMAGE := $(SDK)/u-boot/tools/mkimage

BUILD_DIR := build

FREERTOS_DIR := third_party/FreeRTOS-Kernel
PORT_DIR := $(FREERTOS_DIR)/portable/GCC/ARM_CA53_64_BIT_SRE

CPPFLAGS := \
	-DGUEST \
	-Irtos \
	-I$(FREERTOS_DIR)/include \
	-I$(PORT_DIR)

CFLAGS := \
	-std=gnu11 \
	-march=armv8-a \
	-mgeneral-regs-only \
	-mstrict-align \
	-ffreestanding \
	-fno-builtin \
	-fno-stack-protector \
	-fno-pic \
	-fno-pie \
	-ffixed-x18 \
	-fno-tree-loop-distribute-patterns \
	-O2 \
	-g3 \
	-Wall \
	-Wextra \
	-MMD \
	-MP

ASFLAGS := \
	-march=armv8-a \
	-g3 \
	-MMD \
	-MP

LDFLAGS := \
	-nostdlib \
	-nostartfiles \
	-no-pie \
	-Wl,-T,board/rk3588/linker.ld \
	-Wl,-Map,$(BUILD_DIR)/amp_bootstrap.map \
	-Wl,--build-id=none

C_SRCS := \
	rtos/main.c \
	rtos/irq_timer.c \
	rtos/shared_memory.c \
	rtos/runtime.c \
	$(FREERTOS_DIR)/tasks.c \
	$(FREERTOS_DIR)/queue.c \
	$(FREERTOS_DIR)/list.c \
	$(FREERTOS_DIR)/portable/MemMang/heap_4.c \
	common/amp_message.c \
	common/amp_transport.c \
	rtos/board_led.c \
	rtos/monitor_task.c \
	rtos/control_task.c \
	$(PORT_DIR)/port.c

ASM_SRCS := \
	board/rk3588/startup.S \
	board/rk3588/vector.S \
	$(PORT_DIR)/portASM.S

C_OBJS := $(addprefix $(BUILD_DIR)/,$(C_SRCS:.c=.o))
ASM_OBJS := $(addprefix $(BUILD_DIR)/,$(ASM_SRCS:.S=.o))
OBJS := $(C_OBJS) $(ASM_OBJS)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean amp

all: $(BUILD_DIR)/amp_bootstrap.bin

$(BUILD_DIR)/%.o: %.c Makefile
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S Makefile
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/amp_bootstrap.elf: $(OBJS) board/rk3588/linker.ld Makefile
	$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@

$(BUILD_DIR)/amp_bootstrap.bin: $(BUILD_DIR)/amp_bootstrap.elf
	$(OBJCOPY) -O binary $< $@
	$(OBJDUMP) -d $< > $(BUILD_DIR)/amp_bootstrap.dis

amp: $(BUILD_DIR)/amp.img

$(BUILD_DIR)/amp.img: $(BUILD_DIR)/amp_bootstrap.bin board/uboot/amp.its
	$(MKIMAGE) -f board/uboot/amp.its -E -p 0xe00 $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)