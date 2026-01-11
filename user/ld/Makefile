CC = gcc
AS = as
LD = ld

#ld.so is loaded at fixed addressso  no need for PIC
CFLAGS = -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -m64 -O0 -std=c11 -I../libc/include -nostdlib
ASFLAGS = 
LDFLAGS = -nostdlib -static -T link.ld

OBJS = entry.o ld.o

all: ld.so

entry.o: entry.S
	$(AS) -o $@ $<

ld.o: ld.c
	$(CC) $(CFLAGS) -c -o $@ $<

#build as static executable (interpreter must be self-contained)
ld.so: $(OBJS) link.ld
	$(LD) $(LDFLAGS) -o $@ entry.o ld.o

clean:
	rm -f *.o ld.so

install: ld.so
	cp ld.so ../../initrd/system/libraries/

.PHONY: all clean install
