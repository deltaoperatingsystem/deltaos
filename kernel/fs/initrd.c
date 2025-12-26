#include <fs/initrd.h>
#include <fs/da.h>
#include <fs/tmpfs.h>
#include <fs/fs.h>
#include <boot/db.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <lib/string.h>
#include <lib/io.h>
#include <obj/object.h>

static void *initrd_base = NULL;
static uint64 initrd_size = 0;

//callback for iterating DA entries
static void populate_entry(da_header_t *hdr, da_entry_t *entry, void *ctx) {
    (void)ctx;
    
    const char *path = da_entry_path(hdr, entry);
    if (!path) return;
    
    //skip root directory entry
    if (strcmp(path, "/") == 0) return;
    
    //build full path: /initrd + archive_path
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/initrd%s", path);
    
    uint32 type = da_entry_type(entry);
    
    if (type == DA_TYPE_DIR) {
        //create directory
        if (tmpfs_create_dir(full_path) < 0) {
            printf("[initrd] warn: failed to create dir %s\n", full_path);
        }
    } else if (type == DA_TYPE_FILE) {
        //create file
        if (tmpfs_create(full_path) < 0) {
            printf("[initrd] warn: failed to create file %s\n", full_path);
            return;
        }
        
        //write file contents
        if (entry->size > 0) {
            void *data = da_file_data(hdr, entry);
            if (data) {
                object_t *file = tmpfs_open(full_path);
                if (file) {
                    object_write(file, data, entry->size, 0);
                    object_release(file);
                }
            }
        }
    }
    //symlinks not yet supported in tmpfs
}

void initrd_init(void) {
    struct db_tag_initrd *tag = db_get_initrd();
    if (!tag) {
        puts("[initrd] no initrd present\n");
        return;
    }
    
    //convert physical address to virtual via HHDM
    initrd_base = (void *)P2V(tag->start);
    initrd_size = tag->length;
    
    printf("[initrd] found at phys 0x%lx, size %lu bytes\n", 
           tag->start, (unsigned long)initrd_size);
    
    //validate DA archive
    int err = da_validate(initrd_base, initrd_size);
    if (err != DA_OK) {
        printf("[initrd] invalid DA archive (error %d)\n", err);
        return;
    }
    
    da_header_t *hdr = (da_header_t *)initrd_base;
    printf("[initrd] DA v%04x, %u entries\n", hdr->version, hdr->entry_count);
    
    //create /initrd mount point
    if (tmpfs_create_dir("/initrd") < 0) {
        puts("[initrd] failed to create /initrd mount point\n");
        return;
    }
    
    //populate tmpfs from archive
    da_foreach(hdr, populate_entry, NULL);
    
    printf("[initrd] mounted %u entries to /initrd\n", hdr->entry_count);
}

void *initrd_get_base(void) {
    return initrd_base;
}

uint64 initrd_get_size(void) {
    return initrd_size;
}

void initrd_reclaim(void) {
    if (!initrd_base || !initrd_size) return;
    
    //convert back to physical
    uintptr phys = V2P(initrd_base);
    size pages = initrd_size / PAGE_SIZE;
    if (initrd_size % PAGE_SIZE) pages++;
    
    pmm_free((void *)phys, pages);
    
    printf("[initrd] reclaimed %lu pages\n", (unsigned long)pages);
    
    initrd_base = NULL;
    initrd_size = 0;
}
