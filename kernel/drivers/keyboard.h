#ifndef ARCH_AMD64_DRIVERS_KEYBOARD_H
#define ARCH_AMD64_DRIVERS_KEYBOARD_H

void keyboard_irq(void);
bool get_key(char *c);
bool get_keystate(char c);
void keyboard_queue_interrupt(uint64 pid);
void keyboard_init(void);
void keyboard_start(void);
void keyboard_wait(void);

#endif
