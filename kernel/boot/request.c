#include <boot/db.h>

__attribute__((section(".db_request"), used))
struct db_request_header db_req_hdr = {
    .magic = DB_REQUEST_MAGIC,
    .checksum = 0, //filled by build script
    .version = DB_PROTOCOL_VERSION,
    .header_size = sizeof(struct db_request_header),
    .flags = DB_REQ_FRAMEBUFFER | DB_REQ_MEMORY_MAP | DB_REQ_ACPI,
    .entry_point = DB_ENTRY_USE_FORMAT
};
