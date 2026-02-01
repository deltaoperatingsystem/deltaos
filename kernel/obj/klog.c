#include <obj/klog.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <lib/spinlock.h>
#include <lib/string.h>
#include <fs/fs.h>
#include <arch/cpu.h>

#define KLOG_SIZE 16384  //16KB ring buffer

static char klog_buf[KLOG_SIZE];
static size klog_head = 0;      //write position
static size klog_len = 0;       //current length of data in buffer
static spinlock_t klog_lock = SPINLOCK_INIT;

//append a single character to the log
void klog_putc(char c) {
    irq_state_t flags = arch_irq_save();
    spinlock_acquire(&klog_lock);
    
    klog_buf[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_SIZE;
    if (klog_len < KLOG_SIZE) klog_len++;
    
    spinlock_release(&klog_lock);
    arch_irq_restore(flags);
}

//append a string to the log
void klog_write(const char *s, size len) {
    if (!s || len == 0) return;
    
    irq_state_t flags = arch_irq_save();
    spinlock_acquire(&klog_lock);
    
    for (size i = 0; i < len; i++) {
        klog_buf[klog_head] = s[i];
        klog_head = (klog_head + 1) % KLOG_SIZE;
        if (klog_len < KLOG_SIZE) klog_len++;
    }
    
    spinlock_release(&klog_lock);
    arch_irq_restore(flags);
}

//object read operation - reads from ring buffer
static ssize klog_read_op(object_t *obj, void *buf, size len, size offset) {
    (void)obj;
    
    irq_state_t flags = arch_irq_save();
    spinlock_acquire(&klog_lock);
    
    //clamp offset and length
    if (offset >= klog_len) {
        spinlock_release(&klog_lock);
        arch_irq_restore(flags);
        return 0;
    }
    
    size avail = klog_len - offset;
    if (len > avail) len = avail;
    
    //calculate start position in ring buffer
    //tail is where oldest data is (head - len in ring)
    size tail = (klog_head + KLOG_SIZE - klog_len) % KLOG_SIZE;
    size read_pos = (tail + offset) % KLOG_SIZE;
    
    //copy data out
    char *dst = (char *)buf;
    for (size i = 0; i < len; i++) {
        dst[i] = klog_buf[read_pos];
        read_pos = (read_pos + 1) % KLOG_SIZE;
    }
    
    spinlock_release(&klog_lock);
    arch_irq_restore(flags);
    
    return (ssize)len;
}

static int klog_stat_op(object_t *obj, stat_t *st) {
    (void)obj;
    if (!st) return -1;
    
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_FILE;
    
    irq_state_t flags = arch_irq_save();
    spinlock_acquire(&klog_lock);
    st->size = klog_len;
    spinlock_release(&klog_lock);
    arch_irq_restore(flags);
    
    return 0;
}

static object_ops_t klog_ops = {
    .read = klog_read_op,
    .write = NULL,
    .close = NULL,
    .readdir = NULL,
    .lookup = NULL,
    .stat = klog_stat_op
};

void klog_init(void) {
    object_t *obj = object_create(OBJECT_INFO, &klog_ops, NULL);
    if (obj) {
        ns_register("$kernel/log", obj);
        object_deref(obj);
    }
}
