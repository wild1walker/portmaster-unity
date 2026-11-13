// memledger.c: which library mapped which memory (UA_MEM_LEDGER=1), and glibc's malloc arenas
#pragma once
#include "../loader/so_loader.h"

const so_shim *memledger_table(void);   // mmap/munmap/mremap wrappers for the Android libraries (empty when off)
void memledger_report(void);            // [mem] lines: per library mapped and resident MB; malloc's arenas
