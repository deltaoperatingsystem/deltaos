ARCH ?= amd64
KERNEL := delta.elf

#toolchain
CC := gcc
NASM := nasm
LD := ld

CFLAGS := -Ilib -I. -DARCH_$(shell echo $(ARCH) | tr a-z A-Z) -std=c11 -ffreestanding -fno-stack-protector -fno-pic -m64 -mcmodel=kernel \
          -mno-red-zone \
          -mgeneral-regs-only \
          -Wall -Wextra -O2 -g

NASMFLAGS := -f elf64 -g

LDFLAGS := -nostdlib
LDFLAGS += -T arch/$(ARCH)/linker.ld

#find all C sources in the kernel
ALL_C_SRCS := $(shell find . -name '*.c')

#drivers managed by the registry (modular drivers)
#we exclude core driver infra from this list so it's always included
MODULAR_DRIVERS_ALL := $(shell find drivers -maxdepth 1 -name '*.c' ! -name 'init.c' ! -name 'stubs.c' ! -name 'ps2.c')

include drivers/drivers_enabled.mk

#convert enabled driver names (e.g. keyboard.c) to full paths
ENABLED_DRIVERS_PATHS := $(addprefix drivers/, $(DRIVERS))

#identify which modular drivers are currently DISABLED
DISABLED_DRIVERS_PATHS := $(filter-out $(ENABLED_DRIVERS_PATHS), $(MODULAR_DRIVERS_ALL))

#final C source list so everything minus the explicitly disabled drivers
C_SRCS := $(filter-out $(DISABLED_DRIVERS_PATHS), $(ALL_C_SRCS))
ASM_SRCS := $(shell find . -name '*.asm' ! -name 'trampoline_bin.asm')

ASM_OBJS := $(patsubst %.asm,%.o,$(ASM_SRCS))
C_OBJS := $(patsubst %.c,%.o,$(C_SRCS))
OBJS := $(ASM_OBJS) $(C_OBJS)

#trampoline binary definition and rules
TRAMPOLINE_BIN := arch/amd64/smp/trampoline.bin

.PHONY: all clean

all: drivers/drivers_enabled.mk $(KERNEL)

#generate the driver list
drivers/drivers_enabled.mk: drivers/registry.toml
	../tools/gen_driver_list.py

#build kernel
$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	@python3 ../tools/db_patch.py $@

#trampoline binary must be built first
$(TRAMPOLINE_BIN): arch/amd64/smp/trampoline_bin.asm
	$(NASM) -f bin -o $@ $<

arch/amd64/smp/trampoline.o: arch/amd64/smp/trampoline.asm $(TRAMPOLINE_BIN)
	$(NASM) $(NASMFLAGS) -o $@ $<

#compile C sources
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

#assemble assembly sources
%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $<

clean:
	rm -f $(KERNEL) $(TRAMPOLINE_BIN)
	find . -type f -name "*.o" -delete
	find . -type f -name "drivers_enabled*" -delete
