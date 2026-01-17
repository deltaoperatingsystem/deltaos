extern void _mem_init();
extern void _io_init();

void _lib_init(void) {
    _io_init();
    _mem_init();
}