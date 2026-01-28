.PHONY: all clean ld coreutils libraries

CC := gcc
AS := gcc
LD := ld
AR := ar

# discover lib/*/include dirs and convert to -I flags
LIB_INC_DIRS := $(shell find lib -maxdepth 2 -type d -name include 2>/dev/null)
LIB_INCLUDES := $(patsubst %,-I%,$(LIB_INC_DIRS))

CFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -m64 -c -O2 -Ilibc/include -std=c11 -fPIC $(LIB_INCLUDES)
ASFLAGS := -m64 -c
LDFLAGS := -nostdlib

CRT0 := libc/src/crt0.o

ARCH ?= amd64

LIBC_SRCS := $(shell find libc -name '*.c' | grep -v 'libc/src/arch/')
LIBC_SRCS += $(shell find libc/src/arch/$(ARCH) -name '*.c' 2>/dev/null)
LIBC_OBJS := $(LIBC_SRCS:.c=.o)

SRC_SRCS := $(shell find src -name '*.c' 2>/dev/null)
SRC_OBJS := $(SRC_SRCS:.c=.o)

# find top-level src projects (directory names under src/)
TOPS := $(sort $(foreach f,$(SRC_SRCS),$(firstword $(subst /, ,$(patsubst src/%,%,$(dir $(f)))))))
USER_BINS := $(patsubst %,../initrd/system/binaries/%,$(TOPS))

# find top-level lib projects (directory names under lib/)
LIB_TOPS := $(notdir $(wildcard lib/*))
LIB_SRCS := $(foreach l,$(LIB_TOPS),$(shell find lib/$(l) -name '*.c' 2>/dev/null))
LIB_OBJS := $(LIB_SRCS:.c=.o)
USER_LIBS := $(patsubst %,../initrd/system/libraries/lib%.so,$(LIB_TOPS))

# per-src-project config loader
define LOAD_PROJECT_CONFIG
CFLAGS_$(1) 	:=
LDFLAGS_$(1) 	:=
LDLIBS_$(1) 	:=

-include src/$(1)/.config
endef

$(foreach t,$(TOPS),$(eval $(call LOAD_PROJECT_CONFIG,$t)))

# per-lib-project config loader
define LOAD_LIB_PROJECT_CONFIG
CFLAGS_$(1) 	:=
LDFLAGS_$(1) 	:=
LDLIBS_$(1) 	:=

-include lib/$(1)/.config
endef

$(foreach t,$(LIB_TOPS),$(eval $(call LOAD_LIB_PROJECT_CONFIG,$t)))

all: ld libc.so libraries $(USER_BINS) coreutils

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

# compile src C files with per-project CFLAGS_<project>
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CFLAGS_$(firstword $(subst /, ,$(patsubst src/%,%,$(dir $<))))) -o $@ $<

# link user binaries - no special-case libs
define LINK_USER
../initrd/system/binaries/$(1): $(CRT0) $(filter src/$(1)/%,$(SRC_OBJS)) libc.so
	mkdir -p ../initrd/system/binaries
	$(LD) $(LDFLAGS) $(LDFLAGS_$(1)) \
		-dynamic-linker /system/libraries/ld.so \
		-L. -L ../initrd/system/libraries -o $$@ \
		$(CRT0) $(filter src/$(1)/%,$(SRC_OBJS)) -lc $(LDLIBS_$(1))
endef

$(foreach t,$(TOPS),$(eval $(call LINK_USER,$t)))

# ----------------------
# lib/ project handling
# ----------------------

# compile lib C files with per-project CFLAGS_<project>
lib/%.o: lib/%.c
	$(CC) $(CFLAGS) $(CFLAGS_$(firstword $(subst /, ,$(patsubst lib/%,%,$(dir $<))))) -o $@ $<

# libraries target collects all lib shared objects

# link each lib/<proj> into ../initrd/libraries/lib<proj>.so
define LINK_LIB
../initrd/system/libraries/lib$(1).so: $(filter lib/$(1)/%,$(LIB_OBJS)) libc.so
	mkdir -p ../initrd/system/libraries
	$(LD) -nostdlib -shared $(LDFLAGS) $(LDFLAGS_$(1)) -o $$@ \
		$(filter lib/$(1)/%,$(LIB_OBJS)) $(LDLIBS_$(1))

endef

$(foreach t,$(LIB_TOPS),$(eval $(call LINK_LIB,$t)))

libraries: $(USER_LIBS)

# ld build and copy
ld:
	$(MAKE) -C ld
	mkdir -p ../initrd/system/libraries
	cp ld/ld.so ../initrd/system/libraries/

clean:
	rm -f *.o *.bin libc.a libc.so
	find . -name '*.[s]o' -delete
	rm -f ../initrd/system/libraries/lib*.so
	$(MAKE) -C ld clean
	$(MAKE) -C coreutils clean
