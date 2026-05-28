int iabs(int x) {
    unsigned int u = (unsigned int)x;
    return (int)(x < 0 ? -u : u);
}

long labs(long x) {
    unsigned long u = (unsigned long)x;
    return (long)(x < 0 ? -u : u);
}

int imin(int a, int b) {
    return (a < b) ? a : b;
}

int imax(int a, int b) {
    return (a > b) ? a : b;
}

//integer square root (binary search method)
unsigned int isqrt_int(unsigned int n) {
    if (n == 0) return 0;
    
    unsigned int x = n;
    unsigned int y = (x + 1) / 2;
    
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}