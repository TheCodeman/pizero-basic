# Pi Zero BASIC: bare metal on the Raspberry Pi Zero / Zero W (BCM2835, ARM1176).
#
#   make            builds build/kernel.img (and build/kernel.elf)
#   make qemu       runs it in QEMU's raspi0, the console on this terminal
#   make sdcard     copies kernel.img and config.txt into sdcard/
#
# Needs arm-none-eabi-gcc with newlib (Debian/Ubuntu: gcc-arm-none-eabi
# libnewlib-arm-none-eabi).

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

BUILD   := build
ARCH    := -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -marm
CFLAGS  := $(ARCH) -O2 -g -std=gnu11 -Wall -Wextra -ffunction-sections -fdata-sections \
           -fno-common -Isrc
ASFLAGS := $(ARCH) -g
LDFLAGS := $(ARCH) -nostartfiles -T boot/link.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/kernel.map
LIBS    := -lc -lm -lgcc

SRCS := src/main.c src/console.c $(wildcard src/rpi/*.c)
OBJS := $(BUILD)/start.o $(SRCS:%.c=$(BUILD)/%.o)

all: $(BUILD)/kernel.img

$(BUILD)/kernel.img: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@$(SIZE) $<

$(BUILD)/kernel.elf: $(OBJS) boot/link.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD)/start.o: boot/start.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

qemu: $(BUILD)/kernel.elf
	qemu-system-arm -M raspi0 -kernel $< -serial mon:stdio -nographic

sdcard: $(BUILD)/kernel.img
	cp $(BUILD)/kernel.img sdcard/

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

.PHONY: all qemu sdcard clean
