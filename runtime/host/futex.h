// futex.c: the Android libraries' futex system calls measured (UA_FUTEX_STATS) and, opt-in, waited
// for in user space first (UA_FUTEX_SPIN)
#ifndef FUTEX_H
#define FUTEX_H
#include "../loader/so_loader.h"
// once, before the libraries load: { "syscall", wrapper } when a variable is set, else an empty table
const so_shim *futex_table(void);
#endif
