# initrd
The initrd is a [DeltaArchive](/specs/archive) file, it's location specified by the [delboot.cfg](https://github.com/deltaoperatingsystem/deltaos/blob/main/bootloader/boot/delboot.cfg) file on disk.

## Loading the initrd.da
The initrd is passed by the bootloader using the [DB_TAG_INITRD](/specs/boot/#db_tag_initrd-0x000b) tag. It is then parsed during late-boot (after platform-specific and driver init), and it requires `tmpfs` to be mounted on the `$files` namespace.