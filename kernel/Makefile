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

#find assembly sources excluding the trampoline binary source
ASM_SRCS := $(shell find . -name '*.asm' ! -name 'trampoline_bin.asm')
C_SRCS := $(shell find -name '*.c')

ASM_OBJS := $(patsubst %.asm,%.o,$(ASM_SRCS))
C_OBJS := $(patsubst %.c,%.o,$(C_SRCS))
OBJS := $(ASM_OBJS) $(C_OBJS)

#trampoline binary
TRAMPOLINE_BIN := arch/amd64/smp/trampoline.bin

.PHONY: all clean

all: $(KERNEL)

# Build kernel
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
	find . -type f -name *.o -exec rm {} \;
