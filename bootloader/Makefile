ARCH		= amd64

#target
BOOTLOADER	= BOOTX64.EFI

MINGW_CC	= x86_64-w64-mingw32-gcc
INCLUDES	= -Ilib -Iboot/uefi -Isrc

BOOT_CFLAGS	= -Wall -Wextra -std=c11 \
			  -ffreestanding -fno-stack-protector \
			  -fno-stack-check -fshort-wchar \
			  -mno-red-zone -nostdinc \
			  $(INCLUDES)

BOOT_LDFLAGS = -nostdlib -Wl,-dll -shared \
			   -Wl,--subsystem,10 -e efi_main


BOOT_SRCS	= boot/uefi/main.c \
			  lib/string.c \
			  lib/crc32.c \
			  src/graphics.c \
			  src/console.c \
			  src/file.c \
			  src/menu.c \
			  src/config.c \
			  src/elf.c \
			  src/paging.c
BOOT_OBJS	= $(BOOT_SRCS:.c=.o)

.PHONY: all clean

all: $(BOOTLOADER)

%.o: %.c
	$(MINGW_CC) $(BOOT_CFLAGS) -c $< -o $@

$(BOOTLOADER): $(BOOT_OBJS)
	$(MINGW_CC) $(BOOT_LDFLAGS) $(BOOT_OBJS) -o $@

clean:
	rm -f $(BOOT_OBJS) $(BOOTLOADER)