.PHONY: all clean

CC := gcc
LD := ld

CFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -m64 -c -O2 -I../libc/include -std=c11 -fPIC
LDFLAGS := -nostdlib

CRT0 := ../libc/src/crt0.o

SRCS := $(wildcard *.c)
BINS := $(patsubst %.c,../../initrd/system/binaries/%,$(SRCS))

all: $(BINS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ $<

../../initrd/system/binaries/%: %.o $(CRT0) ../libc.so
	mkdir -p ../../initrd/system/binaries
	$(LD) $(LDFLAGS) -dynamic-linker /system/libraries/ld.so -L.. -o $@ $(CRT0) $< -lc

clean:
	rm -f *.o
