#ifndef FS_FAT32_H
#define FS_FAT32_H

#include <arch/types.h>
#include <obj/object.h>

intptr fat32_mount(object_t *source, const char *target);

#endif
