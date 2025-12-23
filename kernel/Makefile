ARCH ?= amd64
KERNEL := delta.elf

#toolchain
CC := gcc
NASM := nasm
LD := ld

CFLAGS := -Ilib -I. -DARCH_$(shell echo $(ARCH) | tr a-z A-Z) -std=c11 -ffreestanding -fno-stack-protector -fno-pic -m64 -mcmodel=kernel \
          -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
          -Wall -Wextra -O2 -g

NASMFLAGS := -f elf64 -g

LDFLAGS := -nostdlib
LDFLAGS += -T arch/$(ARCH)/linker.ld

ASM_SRCS := $(shell find -name '*.asm')
C_SRCS := $(shell find -name '*.c')

ASM_OBJS := $(patsubst %.asm,%.o,$(ASM_SRCS))
C_OBJS := $(patsubst %.c,%.o,$(C_SRCS))
OBJS := $(ASM_OBJS) $(C_OBJS)

#build kernel
$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	@python3 ../tools/db_patch.py $@

#compile C sources
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

#assemble assembly sources
%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $<

.PHONY: all clean

all: $(KERNEL)

clean:
	rm -f $(KERNEL) $(OBJS)
