.PHONY: all clean

NASM := nasm
LD := ld

all: init.bin

init.o: init.asm
	$(NASM) -f elf64 -o $@ $<

init.bin: init.o
	$(LD) -T link.ld -o $@ $<

clean:
	rm -f *.o *.bin
