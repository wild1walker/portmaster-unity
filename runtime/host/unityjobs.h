// unityjobs.c: Unity 2017.x's wait for a job group sleeps on the render thread instead of spinning (UA_JOB_SLEEP)
#ifndef UNITYJOBS_H
#define UNITYJOBS_H
#include <stddef.h>
#include <stdint.h>
// called by unityhooks_on_load with the relocated libunity.so: hooks JobQueue::WaitForJobGroup when UA_JOB_SLEEP is
// set and the library is one whose job queue was read
void unityjobs_on_load(uintptr_t base, size_t size);
#endif
