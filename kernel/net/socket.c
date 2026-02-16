#include <net/socket.h>
#include <net/tcp.h>
#include <obj/object.h>
#include <obj/handle.h>
#include <obj/kobject.h>
#include <lib/io.h>
#include <lib/string.h>
#include <arch/timer.h>
#include <arch/cpu.h>

/*
 *socket object operations
 *
 *maps the generic object read/write/close to TCP operations
 *- read()  blocks until data is available or timeout
 *- write() sends data on the connection
 *- close() gracefully tears down the TCP connection
 */

static ssize socket_read(object_t *obj, void *buf, size len, size offset) {
    (void)offset;
    tcp_conn_t *conn = (tcp_conn_t *)obj->data;
    if (!conn) return -1;
    
    //if connection is closed and no data buffered, return 0 (EOF)
    if (conn->state != TCP_STATE_ESTABLISHED &&
        conn->state != TCP_STATE_CLOSE_WAIT &&
        conn->rx_len == 0) {
        return 0;
    }
    
    //block until data arrives or timeout (5 seconds)
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;
    uint64 start = arch_timer_get_ticks();
    uint64 timeout = (uint64)freq * 5;
    
    while (conn->rx_len == 0) {
        if (conn->state != TCP_STATE_ESTABLISHED &&
            conn->state != TCP_STATE_CLOSE_WAIT) {
            return 0; //connection closed, EOF
        }
        if (arch_timer_get_ticks() - start >= timeout) {
            return 0; //timeout, no data
        }
        arch_pause();
    }
    
    return (ssize)tcp_read(conn, buf, len);
}

static ssize socket_write(object_t *obj, const void *buf, size len, size offset) {
    (void)offset;
    tcp_conn_t *conn = (tcp_conn_t *)obj->data;
    if (!conn) return -1;
    
    if (conn->state != TCP_STATE_ESTABLISHED) return -1;
    
    int res = tcp_send(conn, buf, len);
    if (res != 0) return -1;
    return (ssize)len;
}

static int socket_close(object_t *obj) {
    tcp_conn_t *conn = (tcp_conn_t *)obj->data;
    if (conn) {
        tcp_close(conn);
    }
    return 0;
}

static object_ops_t socket_ops = {
    .read    = socket_read,
    .write   = socket_write,
    .close   = socket_close,
    .readdir = NULL,
    .lookup  = NULL,
    .stat    = NULL,
    .get_info = NULL,
};

handle_t socket_object_create(tcp_conn_t *conn) {
    if (!conn) return INVALID_HANDLE;
    
    object_t *obj = object_create(OBJECT_SOCKET, &socket_ops, conn);
    if (!obj) return INVALID_HANDLE;
    
    handle_t h = handle_alloc(obj, HANDLE_RIGHTS_DEFAULT);
    //handle_alloc increments refcount so we deref our creation ref
    object_deref(obj);
    
    return h;
}
