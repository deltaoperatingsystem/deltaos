.PHONY: all kernel bootloader tools initrd clean run

all: kernel bootloader initrd

kernel:
	@$(MAKE) --no-print-directory -C $@

bootloader:
	@$(MAKE) --no-print-directory -C $@

tools:
	@$(MAKE) --no-print-directory -C tools/darc

initrd: tools
	@mkdir -p initrd
	@echo "===> Creating initrd.da"
	@./tools/darc/darc create initrd.da initrd

clean:
	@$(MAKE) --no-print-directory -C kernel clean
	@$(MAKE) --no-print-directory -C bootloader clean
	@$(MAKE) --no-print-directory -C tools/darc clean
	@rm -f initrd.da

run: all
	@./run.sh