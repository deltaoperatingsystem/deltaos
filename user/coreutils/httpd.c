#include <io.h>
#include <string.h>
#include <system.h>

int main(int argc, char **argv) {
    bool use_ipv6 = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-6") == 0) {
            use_ipv6 = true;
        } else {
            printf("Usage: httpd [-6]\n");
            return 1;
        }
    }
    
    printf("httpd: listening on port 80 (%s)...\n", use_ipv6 ? "IPv6" : "IPv4");
    
    handle_t listener = use_ipv6 ? tcp_listen_ipv6(80) : tcp_listen(80);
    if (listener < 0) {
        printf("httpd: error: failed to listen on port 80 (%s)\n",
               use_ipv6 ? "IPv6" : "IPv4");
        return 1;
    }
    
    while (1) {
        handle_t client = tcp_accept(listener);
        if (client < 0) {
            printf("httpd: error: accept failed\n");
            continue;
        }
        
        printf("httpd: accepted connection\n");
        
        //read request (simple server we just read one buffer-full)
        char req_buf[1024];
        int n = handle_read(client, req_buf, sizeof(req_buf) - 1);
        if (n > 0) {
            req_buf[n] = '\0';
            //we don't parse headers yet just serve the response
        }
        
        //send HTTP response
        const char *resp = 
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html>\n"
            "<head><title>DeltaOS</title></head>\n"
            "<body>\n"
            "<h1>Hello from DeltaOS!</h1>\n"
            "<p>THis page was server by the httpd util!</p>\n"
            "</body>\n"
            "</html>\n";
            
        handle_write(client, resp, strlen(resp));
        
        //close client connection
        handle_close(client);
        printf("httpd: closed connection\n\n");
    }
    
    handle_close(listener);
    return 0;
}
