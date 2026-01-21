.PHONY: all clean ld libkeyboard coreutils

CC := gcc
AS := gcc
LD := ld
AR := ar

CFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -m64 -c -O2 -Ilibc/include -Ilibkeyboard/include -std=c11 -fPIC
ASFLAGS := -m64 -c
LDFLAGS := -nostdlib

CRT0 := libc/src/crt0.o

LIBC_SRCS := $(shell find libc -name '*.c')
LIBC_OBJS := $(LIBC_SRCS:.c=.o)

SRC_SRCS := $(shell find src -name '*.c' 2>/dev/null)
SRC_OBJS := $(SRC_SRCS:.c=.o)

TOPS := $(sort $(foreach f,$(SRC_SRCS),$(firstword $(subst /, ,$(patsubst src/%,%,$(dir $(f)))))))
USER_BINS := $(patsubst %,../initrd/system/binaries/%,$(TOPS))

#apps that need libkeyboard
KEYBOARD_APPS := shell wm

define LOAD_PROJECT_CONFIG
CFLAGS_$(1) 	:=
LDFLAGS_$(1) 	:=
LDLIBS_$(1) 	:=

-include src/$(1)/.config
endef

$(foreach t,$(TOPS),$(eval $(call LOAD_PROJECT_CONFIG,$t)))

all: ld libkeyboard libc.so $(USER_BINS) coreutils

libkeyboard:
	$(MAKE) -C libkeyboard

coreutils: libc.so
	$(MAKE) -C coreutils

libc/src/crt0.o: libc/src/crt0.S
	$(AS) $(ASFLAGS) -o $@ $<

libc/%.o: libc/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

libc.a: $(LIBC_OBJS)
	$(AR) rcs $@ $^

libc.so: $(LIBC_OBJS)
	$(LD) -nostdlib -shared -o $@ $^
	mkdir -p ../initrd/system/libraries
	cp libc.so ../initrd/system/libraries/

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CFLAGS_$(firstword $(subst /, ,$(patsubst src/%,%,$(dir $<))))) -o $@ $<

#link user binaries - some need libkeyboard
define LINK_USER
../initrd/system/binaries/$(1): $(CRT0) $(filter src/$(1)/%,$(SRC_OBJS)) libc.so $(if $(filter $(1),$(KEYBOARD_APPS)),libkeyboard/libkeyboard.so)
	mkdir -p ../initrd/system/binaries
	$(LD) $(LDFLAGS) $(LDFLAGS_$(1)) \
		-dynamic-linker /system/libraries/ld.so \
		-L. -Llibkeyboard -o $$@ \
		$(CRT0) $(filter src/$(1)/%,$(SRC_OBJS)) \
		$(if $(filter $(1),$(KEYBOARD_APPS)),-lkeyboard) -lc $(LDLIBS_$(1))
endef

$(foreach t,$(TOPS),$(eval $(call LINK_USER,$t)))

ld:
	$(MAKE) -C ld
	mkdir -p ../initrd/system/libraries
	cp ld/ld.so ../initrd/system/libraries/

clean:
	rm -f *.o *.bin libc.a libc.so
	find libc -name '*.o' -delete
	find src -name '*.o' -delete
	$(MAKE) -C ld clean
	$(MAKE) -C libkeyboard clean
	$(MAKE) -C coreutils clean
