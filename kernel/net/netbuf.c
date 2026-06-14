#include <net/netbuf.h>
#include <mm/kheap.h>
#include <lib/string.h>

netbuf_t *netbuf_alloc(size capacity) {
    netbuf_t *nb = kzalloc(sizeof(netbuf_t));
    if (!nb) return NULL;
    
    nb->buf = kmalloc(capacity);
    if (!nb->buf) {
        kfree(nb);
        return NULL;
    }
    
    nb->data = nb->buf;
    nb->len = 0;
    nb->capacity = capacity;
    nb->next = NULL;
    return nb;
}

void netbuf_free(netbuf_t *nb) {
    if (!nb) return;
    if (nb->buf) kfree(nb->buf);
    kfree(nb);
}

void netbuf_reserve(netbuf_t *nb, size headroom) {
    nb->data = nb->buf + headroom;
}

void *netbuf_push(netbuf_t *nb, size len) {
    //guard against data pointer underflowing past the backing buffer
    if (len > (size)(nb->data - nb->buf)) return NULL;
    nb->data -= len;
    nb->len += len;
    return nb->data;
}

void *netbuf_pull(netbuf_t *nb, size len) {
    //guard against stripping more bytes than the packet contains
    if (len > nb->len) return NULL;
    void *old = nb->data;
    nb->data += len;
    nb->len -= len;
    return old;
}

void *netbuf_put(netbuf_t *nb, size len) {
    //guard against appending past the end of the backing buffer
    size used_headroom = (size)(nb->data - nb->buf);
    if (len > nb->capacity - used_headroom - nb->len) return NULL;
    void *tail = nb->data + nb->len;
    nb->len += len;
    return tail;
}
