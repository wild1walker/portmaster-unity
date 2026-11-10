// gen.c - writes darwin_abi.h: macOS's numbers and layouts (errno values, flags, option names,
// struct sizes and field offsets) as the SDK's own headers give them for arm64, so that no shim
// carries a hand-copied constant. Built and run on a Mac: gen.sh.
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <langinfo.h>
#include <locale.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <xlocale.h>
#include <runetype.h>

#define C(name) printf("#define DARWIN_%-28s %lld\n", #name, (long long)(name));
#define X(name) printf("#define DARWIN_%-28s 0x%llx\n", #name, (unsigned long long)(name));
#define XN(tag, v) printf("#define DARWIN_%-28s 0x%llx\n", #tag, (unsigned long long)(v));
#define SZ(type, tag) printf("#define DARWIN_SIZEOF_%-21s %zu\n", #tag, sizeof(type));
#define OFF(type, tag, field) printf("#define DARWIN_%s_%-*s %zu\n", #tag, (int)(27 - sizeof #tag), #field, offsetof(type, field));
#define SIG(tag, init_type, init) { init_type v = init; printf("#define DARWIN_%-28s 0x%lx\n", #tag, *(long *)&v); }
#define H(text) printf("\n// %s\n", text);

int main(void)
{
    printf("// darwin_abi.h - macOS arm64's numbers and layouts. Written by abi/gen.c from the SDK's\n"
           "// headers (do not edit: run abi/gen.sh on a Mac).\n#ifndef UA_DARWIN_ABI_H\n#define UA_DARWIN_ABI_H\n");
#include "gen_errno.inc"
#include "gen_file.inc"
#include "gen_net.inc"
#include "gen_sys.inc"
#include "gen_thread.inc"
    printf("\n#endif\n");
    return 0;
}
