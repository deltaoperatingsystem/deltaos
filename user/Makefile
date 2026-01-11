.PHONY: all clean ld

CC := gcc
AS := gcc
LD := ld
AR := ar

CFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -m64 -c -O2 -Ilibc/include -std=c11 -fPIC
ASFLAGS := -m64 -c
LDFLAGS := -nostdlib

# C runtime object - must be linked first
CRT0 := libc/src/crt0.o

# Auto-detect libc sources and objects (recursively)
LIBC_SRCS := $(shell find libc -name '*.c')
LIBC_OBJS := $(LIBC_SRCS:.c=.o)

all: init.bin ld libc.so

# Compile crt0.S
libc/src/crt0.o: libc/src/crt0.S
	$(AS) $(ASFLAGS) -o $@ $<

# Pattern rule for compiling libc sources
libc/%.o: libc/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

#static library (still useful for static linking)
libc.a: $(LIBC_OBJS)
	$(AR) rcs $@ $^

#shared library
libc.so: $(LIBC_OBJS)
	$(LD) -nostdlib -shared -o $@ $^
	mkdir -p ../initrd/system/libraries
	cp libc.so ../initrd/system/libraries/

init.o: init.c
	$(CC) $(CFLAGS) -o $@ $<

#link dynamically with the interpreter
init.bin: $(CRT0) init.o libc.so
	$(LD) $(LDFLAGS) -dynamic-linker /system/libraries/ld.so -L. -o $@ $(CRT0) init.o -lc
	@echo "Init linked dynamically with interpreter /system/libraries/ld.so"

#build ld.so
ld:
	$(MAKE) -C ld
	mkdir -p ../initrd/system/libraries
	cp ld/ld.so ../initrd/system/libraries/

clean:
	rm -f *.o *.bin libc.a libc.so
	find libc -name '*.o' -delete
	$(MAKE) -C ld clean
