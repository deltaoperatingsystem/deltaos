#include <fs/da.h>
#include <lib/string.h>

int da_validate(void *archive, size archive_size) {
    if (archive_size < sizeof(da_header_t)) {
        return DA_ERR_BOUNDS;
    }
    
    da_header_t *hdr = (da_header_t *)archive;
    
    //check magic
    if (hdr->magic != DA_MAGIC) {
        return DA_ERR_MAGIC;
    }
    
    //check version
    if (hdr->version != 0x0001) {
        return DA_ERR_VERSION;
    }
    
    //bounds check entry table
    if (hdr->entry_off + hdr->entry_count * sizeof(da_entry_t) > archive_size) {
        return DA_ERR_BOUNDS;
    }
    
    //bounds check string table
    if (hdr->strtab_off + hdr->strtab_size > archive_size) {
        return DA_ERR_BOUNDS;
    }
    
    //bounds check data section offset
    if (hdr->data_off > archive_size) {
        return DA_ERR_BOUNDS;
    }
    
    return DA_OK;
}

uint32 da_entry_count(da_header_t *hdr) {
    return hdr->entry_count;
}

da_entry_t *da_get_entry(da_header_t *hdr, uint32 index) {
    if (index >= hdr->entry_count) {
        return NULL;
    }
    da_entry_t *entries = (da_entry_t *)((uint8 *)hdr + hdr->entry_off);
    return &entries[index];
}

const char *da_entry_path(da_header_t *hdr, da_entry_t *entry) {
    char *strtab = (char *)((uint8 *)hdr + hdr->strtab_off);
    if (entry->path_off >= hdr->strtab_size) {
        return NULL;
    }
    return strtab + entry->path_off;
}

void *da_file_data(da_header_t *hdr, da_entry_t *entry) {
    if (da_entry_type(entry) != DA_TYPE_FILE) {
        return NULL;
    }
    uint8 *data_section = (uint8 *)hdr + hdr->data_off;
    return data_section + entry->data_off;
}

uint32 da_hash(const char *path) {
    uint32 hash = 0x811c9dc5;  //FNV offset basis
    while (*path) {
        hash ^= (uint8)*path++;
        hash *= 0x01000193;    //FNV prime
    }
    return hash;
}

da_entry_t *da_find(da_header_t *hdr, const char *path) {
    uint32 target_hash = da_hash(path);
    bool use_hash = (hdr->flags & DA_FLAG_HASHED) != 0;
    
    for (uint32 i = 0; i < hdr->entry_count; i++) {
        da_entry_t *entry = da_get_entry(hdr, i);
        
        //fast path: check hash first if available
        if (use_hash && entry->hash != target_hash) {
            continue;
        }
        
        //verify actual path
        const char *entry_path = da_entry_path(hdr, entry);
        if (entry_path && strcmp(entry_path, path) == 0) {
            return entry;
        }
    }
    
    return NULL;
}

void da_foreach(da_header_t *hdr, da_iter_fn fn, void *ctx) {
    for (uint32 i = 0; i < hdr->entry_count; i++) {
        da_entry_t *entry = da_get_entry(hdr, i);
        fn(hdr, entry, ctx);
    }
}
