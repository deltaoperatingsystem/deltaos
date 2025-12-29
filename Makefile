.PHONY: all kernel bootloader tools user initrd clean run

all: kernel bootloader initrd

kernel:
	@$(MAKE) --no-print-directory -C $@

bootloader:
	@$(MAKE) --no-print-directory -C $@

tools:
	@$(MAKE) --no-print-directory -C tools/darc

user:
	@$(MAKE) --no-print-directory -C $@

initrd: tools user
	@mkdir -p initrd
	@cp user/init.bin initrd/init
	@echo "===> Creating initrd.da"
	@./tools/darc/darc create initrd.da initrd

clean:
	@$(MAKE) --no-print-directory -C kernel clean
	@$(MAKE) --no-print-directory -C bootloader clean
	@$(MAKE) --no-print-directory -C tools/darc clean
	@rm -f initrd.da

run: all
	@./run.sh