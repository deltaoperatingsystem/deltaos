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
    nb->data -= len;
    nb->len += len;
    return nb->data;
}

void *netbuf_pull(netbuf_t *nb, size len) {
    void *old = nb->data;
    nb->data += len;
    nb->len -= len;
    return old;
}

void *netbuf_put(netbuf_t *nb, size len) {
    void *tail = nb->data + nb->len;
    nb->len += len;
    return tail;
}
