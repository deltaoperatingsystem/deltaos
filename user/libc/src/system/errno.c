static int _errno = 0;

int *__errno_location(void) {
    return &_errno;
}
