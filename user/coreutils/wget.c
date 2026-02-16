#include <io.h>
#include <string.h>
#include <system.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: wget <hostname> [path]\n");
        return 1;
    }
    
    const char *hostname = argv[1];
    const char *path = (argc >= 3) ? argv[2] : "/";
    
    printf("Connecting to %s:80...\n", hostname);
    
    handle_t sock = tcp_connect(hostname, 80);
    if (sock < 0) {
        printf("Error: failed to connect to %s\n", hostname);
        return 1;
    }
    
    printf("Connected! Sending GET %s\n", path);
    
    //build HTTP request
    char request[512];
    int reqlen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, hostname);
    
    if (handle_write(sock, request, reqlen) < 0) {
        printf("Error: failed to send request\n");
        handle_close(sock);
        return 1;
    }
    
    //read and print response
    char buf[1024];
    int n;
    while ((n = handle_read(sock, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    
    printf("\n");
    handle_close(sock);
    return 0;
}
