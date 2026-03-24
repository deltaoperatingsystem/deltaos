#include <net/socket.h>
#include <net/tcp.h>
#include <obj/object.h>
#include <obj/handle.h>
#include <obj/kobject.h>
#include <lib/io.h>
#include <lib/string.h>
#include <net/net.h>

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
    
    //if connection is fully closed and no data buffered, return 0 (EOF)
    if (conn->state == TCP_STATE_CLOSED && conn->rx_len == 0) {
        return 0;
    }
    
    //delegate to tcp_read which handles polling, yielding, and timeout
    int result = tcp_read(conn, buf, len);
    if (result < 0) return -1; //error/timeout
    return (ssize)result;
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
