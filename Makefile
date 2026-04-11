.PHONY: all
all: kernel bootloader initrd

.PHONY: kernel
kernel:
	@$(MAKE) --no-print-directory -C $@

.PHONY: bootloader
bootloader:
	@$(MAKE) --no-print-directory -C $@

.PHONY: tools
tools:
	@$(MAKE) --no-print-directory -C tools/darc

.PHONY: user
user:
	@$(MAKE) --no-print-directory -C $@

.PHONY: initrd
initrd: tools user
	@mkdir -p initrd
	@echo "===> Creating initrd.da"
	@./tools/darc/darc create initrd.da initrd

.PHONY: clean
clean:
	@$(MAKE) --no-print-directory -C kernel clean
	@$(MAKE) --no-print-directory -C bootloader clean
	@$(MAKE) --no-print-directory -C user clean
	@$(MAKE) --no-print-directory -C tools/darc clean
	@rm -f initrd.da
	@rm -rf initrd/system initrd/init initrd/wm initrd/data.txt

.PHONY: run
run: all
	@./run.sh

.PHONY: web
web:
	@cd web && python -m http.server 8000