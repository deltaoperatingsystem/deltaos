.PHONY: all clean

CC := gcc
AS := gcc
LD := ld
AR := ar

CFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -m64 -c -O2 -Ilibc -std=c11
ASFLAGS := -m64 -c
LDFLAGS := -T link.ld

# C runtime object - must be linked first
CRT0 := libc/crt0.o

# Auto-detect libc sources and objects (recursively)
LIBC_SRCS := $(shell find libc -name '*.c')
LIBC_OBJS := $(LIBC_SRCS:.c=.o)

all: init.bin

# Compile crt0.S
libc/crt0.o: libc/crt0.S
	$(AS) $(ASFLAGS) -o $@ $<

# Pattern rule for compiling libc sources
libc/%.o: libc/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

libc.a: $(LIBC_OBJS)
	$(AR) rcs $@ $^

init.o: init.c
	$(CC) $(CFLAGS) -o $@ $<

# Link: crt0 first, then init.o, then libc.a
init.bin: $(CRT0) init.o libc.a
	$(LD) $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o *.bin libc.a
	find libc -name '*.o' -delete
