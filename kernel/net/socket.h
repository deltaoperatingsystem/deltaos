#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <obj/object.h>
#include <obj/handle.h>
#include <net/tcp.h>

//create a socket object from a TCP connection, returns a handle
handle_t socket_object_create(tcp_conn_t *conn);

#endif
