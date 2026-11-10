// gen.c - writes darwin_abi.h: macOS's numbers and layouts (errno values, flags, option names,
// struct sizes and field offsets) as the SDK's own headers give them for arm64, so that no shim
// carries a hand-copied constant. Built and run on a Mac: gen.sh.
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <copyfile.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <ifaddrs.h>
#include <langinfo.h>
#include <locale.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <xlocale.h>
#include <runetype.h>

#include "gen.h"

int main(void)
{
    printf("// darwin_abi.h - macOS arm64's numbers and layouts. Written by abi/gen.c from the SDK's\n"
           "// headers (do not edit: run abi/gen.sh on a Mac).\n#ifndef UA_DARWIN_ABI_H\n#define UA_DARWIN_ABI_H\n");
#include "gen_errno.inc"
#include "gen_file.inc"
#include "gen_net.inc"
#include "gen_sys.inc"
#include "gen_thread.inc"
    gen_xopen();
    printf("\n#endif\n");
    return 0;
}
