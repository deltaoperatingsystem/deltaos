#ifndef ARCH_AMD64_SMP_H
#define ARCH_AMD64_SMP_H

#include <arch/amd64/types.h>

//initialize SMP and start all APs
void smp_init(void);

//get number of online CPUs
uint32 smp_cpu_count(void);

//check if a specific CPU has started
bool smp_ap_started(uint32 cpu_index);

//called by AP after entering long mode
void ap_entry(uint32 cpu_index);

//send rescheduling IPI to a specific CPU
void arch_smp_send_resched(uint32 cpu_index);

#endif
