.PHONY: all clean

CC := gcc
LD := ld

CFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -m64 -c -O2 -Iinclude -I../libc/include -std=c11 -fPIC
LDFLAGS := -nostdlib -shared

SRCS := $(shell find src -name '*.c')
OBJS := $(SRCS:.c=.o)

all: libkeyboard.so
	mkdir -p ../../initrd/system/libraries
	cp libkeyboard.so ../../initrd/system/libraries/

src/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

libkeyboard.so: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

clean:
	rm -f libkeyboard.so
	find src -name '*.o' -delete
