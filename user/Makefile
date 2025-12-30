.PHONY: all clean

CC := gcc
LD := ld
AR := ar

CFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -m64 -c -O2 -Ilibc -std=c11
LDFLAGS := -T link.ld

# Auto-detect libc sources and objects (recursively)
LIBC_SRCS := $(shell find libc -name '*.c')
LIBC_OBJS := $(LIBC_SRCS:.c=.o)

all: init.bin

# Pattern rule for compiling libc sources
libc/%.o: libc/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

libc.a: $(LIBC_OBJS)
	$(AR) rcs $@ $^

init.o: init.c
	$(CC) $(CFLAGS) -o $@ $<

init.bin: init.o libc.a
	$(LD) $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o *.bin libc.a
	find libc -name '*.o' -delete
