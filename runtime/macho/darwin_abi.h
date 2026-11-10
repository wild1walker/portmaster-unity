// darwin_abi.h - macOS arm64's numbers and layouts. Written by abi/gen.c from the SDK's
// headers (do not edit: run abi/gen.sh on a Mac).
#ifndef UA_DARWIN_ABI_H
#define UA_DARWIN_ABI_H

// errno
#define DARWIN_EPERM                        1
#define DARWIN_ENOENT                       2
#define DARWIN_ESRCH                        3
#define DARWIN_EINTR                        4
#define DARWIN_EIO                          5
#define DARWIN_ENXIO                        6
#define DARWIN_E2BIG                        7
#define DARWIN_ENOEXEC                      8
#define DARWIN_EBADF                        9
#define DARWIN_ECHILD                       10
#define DARWIN_EDEADLK                      11
#define DARWIN_ENOMEM                       12
#define DARWIN_EACCES                       13
#define DARWIN_EFAULT                       14
#define DARWIN_ENOTBLK                      15
#define DARWIN_EBUSY                        16
#define DARWIN_EEXIST                       17
#define DARWIN_EXDEV                        18
#define DARWIN_ENODEV                       19
#define DARWIN_ENOTDIR                      20
#define DARWIN_EISDIR                       21
#define DARWIN_EINVAL                       22
#define DARWIN_ENFILE                       23
#define DARWIN_EMFILE                       24
#define DARWIN_ENOTTY                       25
#define DARWIN_ETXTBSY                      26
#define DARWIN_EFBIG                        27
#define DARWIN_ENOSPC                       28
#define DARWIN_ESPIPE                       29
#define DARWIN_EROFS                        30
#define DARWIN_EMLINK                       31
#define DARWIN_EPIPE                        32
#define DARWIN_EDOM                         33
#define DARWIN_ERANGE                       34
#define DARWIN_EAGAIN                       35
#define DARWIN_EINPROGRESS                  36
#define DARWIN_EALREADY                     37
#define DARWIN_ENOTSOCK                     38
#define DARWIN_EDESTADDRREQ                 39
#define DARWIN_EMSGSIZE                     40
#define DARWIN_EPROTOTYPE                   41
#define DARWIN_ENOPROTOOPT                  42
#define DARWIN_EPROTONOSUPPORT              43
#define DARWIN_ESOCKTNOSUPPORT              44
#define DARWIN_ENOTSUP                      45
#define DARWIN_EPFNOSUPPORT                 46
#define DARWIN_EAFNOSUPPORT                 47
#define DARWIN_EADDRINUSE                   48
#define DARWIN_EADDRNOTAVAIL                49
#define DARWIN_ENETDOWN                     50
#define DARWIN_ENETUNREACH                  51
#define DARWIN_ENETRESET                    52
#define DARWIN_ECONNABORTED                 53
#define DARWIN_ECONNRESET                   54
#define DARWIN_ENOBUFS                      55
#define DARWIN_EISCONN                      56
#define DARWIN_ENOTCONN                     57
#define DARWIN_ESHUTDOWN                    58
#define DARWIN_ETOOMANYREFS                 59
#define DARWIN_ETIMEDOUT                    60
#define DARWIN_ECONNREFUSED                 61
#define DARWIN_ELOOP                        62
#define DARWIN_ENAMETOOLONG                 63
#define DARWIN_EHOSTDOWN                    64
#define DARWIN_EHOSTUNREACH                 65
#define DARWIN_ENOTEMPTY                    66
#define DARWIN_EPROCLIM                     67
#define DARWIN_EUSERS                       68
#define DARWIN_EDQUOT                       69
#define DARWIN_ESTALE                       70
#define DARWIN_EREMOTE                      71
#define DARWIN_EBADRPC                      72
#define DARWIN_ERPCMISMATCH                 73
#define DARWIN_EPROGUNAVAIL                 74
#define DARWIN_EPROGMISMATCH                75
#define DARWIN_EPROCUNAVAIL                 76
#define DARWIN_ENOLCK                       77
#define DARWIN_ENOSYS                       78
#define DARWIN_EFTYPE                       79
#define DARWIN_EAUTH                        80
#define DARWIN_ENEEDAUTH                    81
#define DARWIN_EPWROFF                      82
#define DARWIN_EDEVERR                      83
#define DARWIN_EOVERFLOW                    84
#define DARWIN_EBADEXEC                     85
#define DARWIN_EBADARCH                     86
#define DARWIN_ESHLIBVERS                   87
#define DARWIN_EBADMACHO                    88
#define DARWIN_ECANCELED                    89
#define DARWIN_EIDRM                        90
#define DARWIN_ENOMSG                       91
#define DARWIN_EILSEQ                       92
#define DARWIN_ENOATTR                      93
#define DARWIN_EBADMSG                      94
#define DARWIN_EMULTIHOP                    95
#define DARWIN_ENODATA                      96
#define DARWIN_ENOLINK                      97
#define DARWIN_ENOSR                        98
#define DARWIN_ENOSTR                       99
#define DARWIN_EPROTO                       100
#define DARWIN_ETIME                        101
#define DARWIN_EOPNOTSUPP                   102
#define DARWIN_ENOPOLICY                    103
#define DARWIN_ENOTRECOVERABLE              104
#define DARWIN_EOWNERDEAD                   105
#define DARWIN_EQFULL                       106
#define DARWIN_ELAST                        106

// strerror's texts, by number: all the running libc knows (it may know more than the SDK's ELAST)
#define DARWIN_ERRTEXTS { \
    "Undefined error: 0", \
    "Operation not permitted", \
    "No such file or directory", \
    "No such process", \
    "Interrupted system call", \
    "Input/output error", \
    "Device not configured", \
    "Argument list too long", \
    "Exec format error", \
    "Bad file descriptor", \
    "No child processes", \
    "Resource deadlock avoided", \
    "Cannot allocate memory", \
    "Permission denied", \
    "Bad address", \
    "Block device required", \
    "Resource busy", \
    "File exists", \
    "Cross-device link", \
    "Operation not supported by device", \
    "Not a directory", \
    "Is a directory", \
    "Invalid argument", \
    "Too many open files in system", \
    "Too many open files", \
    "Inappropriate ioctl for device", \
    "Text file busy", \
    "File too large", \
    "No space left on device", \
    "Illegal seek", \
    "Read-only file system", \
    "Too many links", \
    "Broken pipe", \
    "Numerical argument out of domain", \
    "Result too large", \
    "Resource temporarily unavailable", \
    "Operation now in progress", \
    "Operation already in progress", \
    "Socket operation on non-socket", \
    "Destination address required", \
    "Message too long", \
    "Protocol wrong type for socket", \
    "Protocol not available", \
    "Protocol not supported", \
    "Socket type not supported", \
    "Operation not supported", \
    "Protocol family not supported", \
    "Address family not supported by protocol family", \
    "Address already in use", \
    "Can't assign requested address", \
    "Network is down", \
    "Network is unreachable", \
    "Network dropped connection on reset", \
    "Software caused connection abort", \
    "Connection reset by peer", \
    "No buffer space available", \
    "Socket is already connected", \
    "Socket is not connected", \
    "Can't send after socket shutdown", \
    "Too many references: can't splice", \
    "Operation timed out", \
    "Connection refused", \
    "Too many levels of symbolic links", \
    "File name too long", \
    "Host is down", \
    "No route to host", \
    "Directory not empty", \
    "Too many processes", \
    "Too many users", \
    "Disc quota exceeded", \
    "Stale NFS file handle", \
    "Too many levels of remote in path", \
    "RPC struct is bad", \
    "RPC version wrong", \
    "RPC prog. not avail", \
    "Program version wrong", \
    "Bad procedure for program", \
    "No locks available", \
    "Function not implemented", \
    "Inappropriate file type or format", \
    "Authentication error", \
    "Need authenticator", \
    "Device power is off", \
    "Device error", \
    "Value too large to be stored in data type", \
    "Bad executable (or shared library)", \
    "Bad CPU type in executable", \
    "Shared library version mismatch", \
    "Malformed Mach-o file", \
    "Operation canceled", \
    "Identifier removed", \
    "No message of desired type", \
    "Illegal byte sequence", \
    "Attribute not found", \
    "Bad message", \
    "EMULTIHOP (Reserved)", \
    "No message available on STREAM", \
    "ENOLINK (Reserved)", \
    "No STREAM resources", \
    "Not a STREAM", \
    "Protocol error", \
    "STREAM ioctl timeout", \
    "Operation not supported on socket", \
    "Policy not found", \
    "State not recoverable", \
    "Previous owner died", \
    "Interface output queue is full", \
    "Capabilities insufficient", \
}

// open, fcntl, ioctl, mmap
#define DARWIN_O_ACCMODE                    0x3
#define DARWIN_O_NONBLOCK                   0x4
#define DARWIN_O_APPEND                     0x8
#define DARWIN_O_SHLOCK                     0x10
#define DARWIN_O_EXLOCK                     0x20
#define DARWIN_O_ASYNC                      0x40
#define DARWIN_O_SYNC                       0x80
#define DARWIN_O_NOFOLLOW                   0x100
#define DARWIN_O_CREAT                      0x200
#define DARWIN_O_TRUNC                      0x400
#define DARWIN_O_EXCL                       0x800
#define DARWIN_O_EVTONLY                    0x8000
#define DARWIN_O_NOCTTY                     0x20000
#define DARWIN_O_DIRECTORY                  0x100000
#define DARWIN_O_SYMLINK                    0x200000
#define DARWIN_O_DSYNC                      0x400000
#define DARWIN_O_CLOEXEC                    0x1000000
#define DARWIN_O_NOFOLLOW_ANY               0x20000000
#define DARWIN_O_RDONLY                     0
#define DARWIN_O_WRONLY                     1
#define DARWIN_O_RDWR                       2
#define DARWIN_MAXPATHLEN                   1024
#define DARWIN_F_DUPFD                      0
#define DARWIN_F_GETFD                      1
#define DARWIN_F_SETFD                      2
#define DARWIN_F_GETFL                      3
#define DARWIN_F_SETFL                      4
#define DARWIN_F_GETOWN                     5
#define DARWIN_F_SETOWN                     6
#define DARWIN_F_GETLK                      7
#define DARWIN_F_SETLK                      8
#define DARWIN_F_SETLKW                     9
#define DARWIN_F_RDAHEAD                    45
#define DARWIN_F_NOCACHE                    48
#define DARWIN_F_GETPATH                    50
#define DARWIN_F_FULLFSYNC                  51
#define DARWIN_F_DUPFD_CLOEXEC              67
#define DARWIN_F_PREALLOCATE                42
#define DARWIN_F_BARRIERFSYNC               85
#define DARWIN_F_RDLCK                      1
#define DARWIN_F_UNLCK                      2
#define DARWIN_F_WRLCK                      3
#define DARWIN_FD_CLOEXEC                   1
#define DARWIN_SIZEOF_FLOCK                 24
#define DARWIN_FLOCK_l_start               0
#define DARWIN_FLOCK_l_len                 8
#define DARWIN_FLOCK_l_pid                 16
#define DARWIN_FLOCK_l_type                20
#define DARWIN_FLOCK_l_whence              22
#define DARWIN_SIZEOF_FSTORE                32
#define DARWIN_FSTORE_fst_flags            0
#define DARWIN_FSTORE_fst_posmode          4
#define DARWIN_FSTORE_fst_offset           8
#define DARWIN_FSTORE_fst_length           16
#define DARWIN_FSTORE_fst_bytesalloc       24
#define DARWIN_F_PEOFPOSMODE                3
#define DARWIN_F_VOLPOSMODE                 4
#define DARWIN_F_ALLOCATECONTIG             0x2
#define DARWIN_F_ALLOCATEALL                0x4
#define DARWIN_FIONREAD                     0x4004667f
#define DARWIN_FIONBIO                      0x8004667e
#define DARWIN_FIOCLEX                      0x20006601
#define DARWIN_FIONCLEX                     0x20006602
#define DARWIN_FIOASYNC                     0x8004667d
#define DARWIN_TIOCGWINSZ                   0x40087468
#define DARWIN_PROT_READ                    0x1
#define DARWIN_PROT_WRITE                   0x2
#define DARWIN_PROT_EXEC                    0x4
#define DARWIN_MAP_SHARED                   0x1
#define DARWIN_MAP_PRIVATE                  0x2
#define DARWIN_MAP_FIXED                    0x10
#define DARWIN_MAP_NORESERVE                0x40
#define DARWIN_MAP_JIT                      0x800
#define DARWIN_MAP_ANON                     0x1000
#define DARWIN_MAP_NOCACHE                  0x400
#define DARWIN_MAP_HASSEMAPHORE             0x200
#define DARWIN_MAP_RENAME                   0x20
#define DARWIN_MAP_NOEXTEND                 0x100
#define DARWIN_MADV_NORMAL                  0
#define DARWIN_MADV_RANDOM                  1
#define DARWIN_MADV_SEQUENTIAL              2
#define DARWIN_MADV_WILLNEED                3
#define DARWIN_MADV_DONTNEED                4
#define DARWIN_MADV_FREE                    5
#define DARWIN_MADV_FREE_REUSABLE           7
#define DARWIN_MADV_FREE_REUSE              8
#define DARWIN_MS_ASYNC                     0x1
#define DARWIN_MS_INVALIDATE                0x2
#define DARWIN_MS_SYNC                      0x10
#define DARWIN_F_OK                         0
#define DARWIN_X_OK                         1
#define DARWIN_W_OK                         2
#define DARWIN_R_OK                         4
#define DARWIN_SEEK_SET                     0
#define DARWIN_SEEK_CUR                     1
#define DARWIN_SEEK_END                     2
#define DARWIN_SEEK_HOLE                    3
#define DARWIN_SEEK_DATA                    4
#define DARWIN_AT_FDCWD                     0xfffffffffffffffe
#define DARWIN_LOCK_SH                      1
#define DARWIN_LOCK_EX                      2
#define DARWIN_LOCK_NB                      4
#define DARWIN_LOCK_UN                      8

// struct stat, struct dirent
#define DARWIN_SIZEOF_STAT                  144
#define DARWIN_STAT_st_dev                 0
#define DARWIN_STAT_st_mode                4
#define DARWIN_STAT_st_nlink               6
#define DARWIN_STAT_st_ino                 8
#define DARWIN_STAT_st_uid                 16
#define DARWIN_STAT_st_gid                 20
#define DARWIN_STAT_st_rdev                24
#define DARWIN_STAT_st_atimespec           32
#define DARWIN_STAT_st_mtimespec           48
#define DARWIN_STAT_st_ctimespec           64
#define DARWIN_STAT_st_birthtimespec       80
#define DARWIN_STAT_st_size                96
#define DARWIN_STAT_st_blocks              104
#define DARWIN_STAT_st_blksize             112
#define DARWIN_STAT_st_flags               116
#define DARWIN_STAT_st_gen                 120
#define DARWIN_SIZEOF_DIRENT                1048
#define DARWIN_DIRENT_d_ino                0
#define DARWIN_DIRENT_d_seekoff            8
#define DARWIN_DIRENT_d_reclen             16
#define DARWIN_DIRENT_d_namlen             18
#define DARWIN_DIRENT_d_type               20
#define DARWIN_DIRENT_d_name               21
#define DARWIN_DT_UNKNOWN                   0
#define DARWIN_DT_FIFO                      1
#define DARWIN_DT_CHR                       2
#define DARWIN_DT_DIR                       4
#define DARWIN_DT_BLK                       6
#define DARWIN_DT_REG                       8
#define DARWIN_DT_LNK                       10
#define DARWIN_DT_SOCK                      12
#define DARWIN_DT_WHT                       14
#define DARWIN_S_IFMT                       0xf000
#define DARWIN_S_IFIFO                      0x1000
#define DARWIN_S_IFCHR                      0x2000
#define DARWIN_S_IFDIR                      0x4000
#define DARWIN_S_IFBLK                      0x6000
#define DARWIN_S_IFREG                      0x8000
#define DARWIN_S_IFLNK                      0xa000
#define DARWIN_S_IFSOCK                     0xc000

// poll, select
#define DARWIN_POLLIN                       0x1
#define DARWIN_POLLPRI                      0x2
#define DARWIN_POLLOUT                      0x4
#define DARWIN_POLLRDNORM                   0x40
#define DARWIN_POLLWRNORM                   0x4
#define DARWIN_POLLRDBAND                   0x80
#define DARWIN_POLLWRBAND                   0x100
#define DARWIN_POLLERR                      0x8
#define DARWIN_POLLHUP                      0x10
#define DARWIN_POLLNVAL                     0x20
#define DARWIN_SIZEOF_NFDS_T                4
#define DARWIN_SIZEOF_POLLFD                8
#define DARWIN_SIZEOF_FD_SET                128
#define DARWIN_FD_SETSIZE                   1024
#define DARWIN_SIZEOF_TIMEVAL               16
#define DARWIN_TIMEVAL_tv_usec             8
#define DARWIN_SIZEOF_TV_USEC               4
#define DARWIN_SIZEOF_TIMESPEC              16

// termios
#define DARWIN_SIZEOF_TERMIOS               72
#define DARWIN_SIZEOF_TCFLAG_T              8
#define DARWIN_TERMIOS_c_cc                32
#define DARWIN_NCCS                         20

// the *at calls, statvfs, utimensat, copyfile
#define DARWIN_AT_EACCESS                   0x10
#define DARWIN_AT_SYMLINK_NOFOLLOW          0x20
#define DARWIN_AT_SYMLINK_FOLLOW            0x40
#define DARWIN_AT_REMOVEDIR                 0x80
#define DARWIN_SIZEOF_STATVFS               64
#define DARWIN_STATVFS_f_bsize             0
#define DARWIN_STATVFS_f_frsize            8
#define DARWIN_STATVFS_f_blocks            16
#define DARWIN_STATVFS_f_bfree             20
#define DARWIN_STATVFS_f_bavail            24
#define DARWIN_STATVFS_f_files             28
#define DARWIN_STATVFS_f_ffree             32
#define DARWIN_STATVFS_f_favail            36
#define DARWIN_STATVFS_f_fsid              40
#define DARWIN_STATVFS_f_flag              48
#define DARWIN_STATVFS_f_namemax           56
#define DARWIN_SIZEOF_FSBLKCNT_T            4
#define DARWIN_SIZEOF_FSFILCNT_T            4
#define DARWIN_ST_RDONLY                    0x1
#define DARWIN_ST_NOSUID                    0x2
#define DARWIN_UTIME_NOW                    0xffffffffffffffff
#define DARWIN_UTIME_OMIT                   0xfffffffffffffffe
#define DARWIN_COPYFILE_ACL                 0x1
#define DARWIN_COPYFILE_STAT                0x2
#define DARWIN_COPYFILE_XATTR               0x4
#define DARWIN_COPYFILE_DATA                0x8
#define DARWIN_COPYFILE_SECURITY            0x3
#define DARWIN_COPYFILE_METADATA            0x7
#define DARWIN_COPYFILE_ALL                 0xf
#define DARWIN_COPYFILE_EXCL                0x20000
#define DARWIN_COPYFILE_NOFOLLOW_SRC        0x40000
#define DARWIN_COPYFILE_NOFOLLOW_DST        0x80000
#define DARWIN_COPYFILE_UNLINK              0x200000

// stdio: FILE as the header's macros read it
#define DARWIN_SIZEOF_FILE                  152
#define DARWIN_FILE__p                     0
#define DARWIN_FILE__r                     8
#define DARWIN_FILE__w                     12
#define DARWIN_FILE__flags                 16
#define DARWIN_FILE__file                  18
#define DARWIN_FILE__bf                    24
#define DARWIN_FILE__lbfsize               40
#define DARWIN_FILE__cookie                48
#define DARWIN___SLBF                       0x1
#define DARWIN___SNBF                       0x2
#define DARWIN___SRD                        0x4
#define DARWIN___SWR                        0x8
#define DARWIN___SRW                        0x10
#define DARWIN___SEOF                       0x20
#define DARWIN___SERR                       0x40
#define DARWIN_LOG_EMERG                    0
#define DARWIN_LOG_ALERT                    1
#define DARWIN_LOG_CRIT                     2
#define DARWIN_LOG_ERR                      3
#define DARWIN_LOG_WARNING                  4
#define DARWIN_LOG_NOTICE                   5
#define DARWIN_LOG_INFO                     6
#define DARWIN_LOG_DEBUG                    7

// sockets
#define DARWIN_AF_UNSPEC                    0
#define DARWIN_AF_UNIX                      1
#define DARWIN_AF_INET                      2
#define DARWIN_AF_INET6                     30
#define DARWIN_AF_LINK                      18
#define DARWIN_AF_ROUTE                     17
#define DARWIN_SOCK_STREAM                  1
#define DARWIN_SOCK_DGRAM                   2
#define DARWIN_SOCK_RAW                     3
#define DARWIN_SOCK_SEQPACKET               5
#define DARWIN_SOL_SOCKET                   0xffff
#define DARWIN_SO_DEBUG                     0x1
#define DARWIN_SO_ACCEPTCONN                0x2
#define DARWIN_SO_REUSEADDR                 0x4
#define DARWIN_SO_KEEPALIVE                 0x8
#define DARWIN_SO_DONTROUTE                 0x10
#define DARWIN_SO_BROADCAST                 0x20
#define DARWIN_SO_USELOOPBACK               0x40
#define DARWIN_SO_LINGER                    0x80
#define DARWIN_SO_OOBINLINE                 0x100
#define DARWIN_SO_REUSEPORT                 0x200
#define DARWIN_SO_TIMESTAMP                 0x400
#define DARWIN_SO_SNDBUF                    0x1001
#define DARWIN_SO_RCVBUF                    0x1002
#define DARWIN_SO_SNDLOWAT                  0x1003
#define DARWIN_SO_RCVLOWAT                  0x1004
#define DARWIN_SO_SNDTIMEO                  0x1005
#define DARWIN_SO_RCVTIMEO                  0x1006
#define DARWIN_SO_ERROR                     0x1007
#define DARWIN_SO_TYPE                      0x1008
#define DARWIN_SO_NOSIGPIPE                 0x1022
#define DARWIN_SO_NREAD                     0x1020
#define DARWIN_SO_LINGER_SEC                0x1080
#define DARWIN_IPPROTO_IP                   0
#define DARWIN_IPPROTO_ICMP                 1
#define DARWIN_IPPROTO_TCP                  6
#define DARWIN_IPPROTO_UDP                  17
#define DARWIN_IPPROTO_IPV6                 41
#define DARWIN_IPPROTO_ICMPV6               58
#define DARWIN_IP_OPTIONS                   1
#define DARWIN_IP_HDRINCL                   2
#define DARWIN_IP_TOS                       3
#define DARWIN_IP_TTL                       4
#define DARWIN_IP_MULTICAST_IF              9
#define DARWIN_IP_MULTICAST_TTL             10
#define DARWIN_IP_MULTICAST_LOOP            11
#define DARWIN_IP_ADD_MEMBERSHIP            12
#define DARWIN_IP_DROP_MEMBERSHIP           13
#define DARWIN_IP_PKTINFO                   26
#define DARWIN_IP_RECVTOS                   27
#define DARWIN_IP_DONTFRAG                  28
#define DARWIN_IP_BOUND_IF                  25
#define DARWIN_IP_ADD_SOURCE_MEMBERSHIP     70
#define DARWIN_IP_DROP_SOURCE_MEMBERSHIP    71
#define DARWIN_IP_BLOCK_SOURCE              72
#define DARWIN_IP_UNBLOCK_SOURCE            73
#define DARWIN_IP_RECVTTL                   24
#define DARWIN_IPV6_UNICAST_HOPS            4
#define DARWIN_IPV6_MULTICAST_IF            9
#define DARWIN_IPV6_MULTICAST_HOPS          10
#define DARWIN_IPV6_MULTICAST_LOOP          11
#define DARWIN_IPV6_JOIN_GROUP              12
#define DARWIN_IPV6_LEAVE_GROUP             13
#define DARWIN_IPV6_V6ONLY                  27
#define DARWIN_IPV6_BOUND_IF                125
#define DARWIN_IPV6_TCLASS                  36
#define DARWIN_TCP_NODELAY                  1
#define DARWIN_TCP_MAXSEG                   2
#define DARWIN_TCP_KEEPALIVE                16
#define DARWIN_TCP_KEEPINTVL                257
#define DARWIN_TCP_KEEPCNT                  258
#define DARWIN_TCP_NOPUSH                   4
#define DARWIN_MSG_OOB                      0x1
#define DARWIN_MSG_PEEK                     0x2
#define DARWIN_MSG_DONTROUTE                0x4
#define DARWIN_MSG_EOR                      0x8
#define DARWIN_MSG_TRUNC                    0x10
#define DARWIN_MSG_CTRUNC                   0x20
#define DARWIN_MSG_WAITALL                  0x40
#define DARWIN_MSG_DONTWAIT                 0x80
#define DARWIN_MSG_NOSIGNAL                 0x80000
#define DARWIN_SHUT_RD                      0
#define DARWIN_SHUT_WR                      1
#define DARWIN_SHUT_RDWR                    2
#define DARWIN_SOMAXCONN                    128
#define DARWIN_SCM_RIGHTS                   1
#define DARWIN_SIZEOF_SOCKADDR              16
#define DARWIN_SOCKADDR_sa_len             0
#define DARWIN_SOCKADDR_sa_family          1
#define DARWIN_SIZEOF_SOCKADDR_IN           16
#define DARWIN_SIZEOF_SOCKADDR_IN6          28
#define DARWIN_SIZEOF_SOCKADDR_UN           106
#define DARWIN_SIZEOF_SOCKADDR_STORAGE      128
#define DARWIN_SOCKADDR_UN_sun_path        2
#define DARWIN_SIZEOF_SOCKADDR_DL           20
#define DARWIN_SOCKADDR_DL_sdl_index       2
#define DARWIN_SOCKADDR_DL_sdl_type        4
#define DARWIN_SOCKADDR_DL_sdl_nlen        5
#define DARWIN_SOCKADDR_DL_sdl_alen        6
#define DARWIN_SOCKADDR_DL_sdl_data        8
#define DARWIN_SIZEOF_MSGHDR                48
#define DARWIN_MSGHDR_msg_name             0
#define DARWIN_MSGHDR_msg_namelen          8
#define DARWIN_MSGHDR_msg_iov              16
#define DARWIN_MSGHDR_msg_iovlen           24
#define DARWIN_MSGHDR_msg_control          32
#define DARWIN_MSGHDR_msg_controllen       40
#define DARWIN_MSGHDR_msg_flags            44
#define DARWIN_SIZEOF_CMSGHDR               12
#define DARWIN_CMSGHDR_cmsg_level          4
#define DARWIN_CMSGHDR_cmsg_type           8
#define DARWIN_SIZEOF_LINGER                8
#define DARWIN_SIZEOF_IP_MREQ               8
#define DARWIN_SIZEOF_IPV6_MREQ             20
#define DARWIN_SIZEOF_IN_PKTINFO            12
#define DARWIN_IN_PKTINFO_ipi_ifindex      0
#define DARWIN_IN_PKTINFO_ipi_spec_dst     4
#define DARWIN_IN_PKTINFO_ipi_addr         8
#define DARWIN_SIZEOF_IP_MREQ_SOURCE        12
#define DARWIN_IP_MREQ_SOURCE_imr_multiaddr 0
#define DARWIN_IP_MREQ_SOURCE_imr_sourceaddr 4
#define DARWIN_IP_MREQ_SOURCE_imr_interface 8

// getaddrinfo, getnameinfo, getifaddrs
#define DARWIN_SIZEOF_ADDRINFO              48
#define DARWIN_ADDRINFO_ai_flags           0
#define DARWIN_ADDRINFO_ai_family          4
#define DARWIN_ADDRINFO_ai_socktype        8
#define DARWIN_ADDRINFO_ai_protocol        12
#define DARWIN_ADDRINFO_ai_addrlen         16
#define DARWIN_ADDRINFO_ai_canonname       24
#define DARWIN_ADDRINFO_ai_addr            32
#define DARWIN_ADDRINFO_ai_next            40
#define DARWIN_AI_PASSIVE                   0x1
#define DARWIN_AI_CANONNAME                 0x2
#define DARWIN_AI_NUMERICHOST               0x4
#define DARWIN_AI_NUMERICSERV               0x1000
#define DARWIN_AI_ALL                       0x100
#define DARWIN_AI_V4MAPPED_CFG              0x200
#define DARWIN_AI_ADDRCONFIG                0x400
#define DARWIN_AI_V4MAPPED                  0x800
#define DARWIN_AI_DEFAULT                   0x600
#define DARWIN_EAI_ADDRFAMILY               1
#define DARWIN_EAI_AGAIN                    2
#define DARWIN_EAI_BADFLAGS                 3
#define DARWIN_EAI_FAIL                     4
#define DARWIN_EAI_FAMILY                   5
#define DARWIN_EAI_MEMORY                   6
#define DARWIN_EAI_NODATA                   7
#define DARWIN_EAI_NONAME                   8
#define DARWIN_EAI_SERVICE                  9
#define DARWIN_EAI_SOCKTYPE                 10
#define DARWIN_EAI_SYSTEM                   11
#define DARWIN_EAI_BADHINTS                 12
#define DARWIN_EAI_PROTOCOL                 13
#define DARWIN_EAI_OVERFLOW                 14
#define DARWIN_NI_NOFQDN                    0x1
#define DARWIN_NI_NUMERICHOST               0x2
#define DARWIN_NI_NAMEREQD                  0x4
#define DARWIN_NI_NUMERICSERV               0x8
#define DARWIN_NI_NUMERICSCOPE              0x100
#define DARWIN_NI_DGRAM                     0x10
#define DARWIN_SIZEOF_IFADDRS               56
#define DARWIN_IFADDRS_ifa_next            0
#define DARWIN_IFADDRS_ifa_name            8
#define DARWIN_IFADDRS_ifa_flags           16
#define DARWIN_IFADDRS_ifa_addr            24
#define DARWIN_IFADDRS_ifa_netmask         32
#define DARWIN_IFADDRS_ifa_dstaddr         40
#define DARWIN_IFADDRS_ifa_data            48
#define DARWIN_IFF_UP                       0x1
#define DARWIN_IFF_BROADCAST                0x2
#define DARWIN_IFF_LOOPBACK                 0x8
#define DARWIN_IFF_POINTOPOINT              0x10
#define DARWIN_IFF_RUNNING                  0x40
#define DARWIN_IFF_NOARP                    0x80
#define DARWIN_IFF_PROMISC                  0x100
#define DARWIN_IFF_ALLMULTI                 0x200
#define DARWIN_IFF_MULTICAST                0x8000

// sysconf, sysctl, resources, uname
#define DARWIN__SC_ARG_MAX                  1
#define DARWIN__SC_CHILD_MAX                2
#define DARWIN__SC_CLK_TCK                  3
#define DARWIN__SC_NGROUPS_MAX              4
#define DARWIN__SC_OPEN_MAX                 5
#define DARWIN__SC_PAGESIZE                 29
#define DARWIN__SC_NPROCESSORS_CONF         57
#define DARWIN__SC_NPROCESSORS_ONLN         58
#define DARWIN__SC_PHYS_PAGES               200
#define DARWIN__SC_GETPW_R_SIZE_MAX         71
#define DARWIN__SC_GETGR_R_SIZE_MAX         70
#define DARWIN__SC_HOST_NAME_MAX            72
#define DARWIN__SC_LOGIN_NAME_MAX           73
#define DARWIN__SC_THREAD_STACK_MIN         93
#define DARWIN__SC_IOV_MAX                  56
#define DARWIN__SC_LINE_MAX                 15
#define DARWIN_CTL_KERN                     1
#define DARWIN_CTL_VM                       2
#define DARWIN_CTL_NET                      4
#define DARWIN_CTL_HW                       6
#define DARWIN_KERN_OSTYPE                  1
#define DARWIN_KERN_OSRELEASE               2
#define DARWIN_KERN_VERSION                 4
#define DARWIN_KERN_ARGMAX                  8
#define DARWIN_KERN_HOSTNAME                10
#define DARWIN_KERN_PROC                    14
#define DARWIN_KERN_BOOTTIME                21
#define DARWIN_KERN_PROCARGS2               49
#define DARWIN_KERN_OSVERSION               65
#define DARWIN_KERN_PROC_PID                1
#define DARWIN_HW_MACHINE                   1
#define DARWIN_HW_MODEL                     2
#define DARWIN_HW_NCPU                      3
#define DARWIN_HW_PHYSMEM                   5
#define DARWIN_HW_USERMEM                   6
#define DARWIN_HW_PAGESIZE                  7
#define DARWIN_HW_MEMSIZE                   24
#define DARWIN_HW_AVAILCPU                  25
#define DARWIN_RLIMIT_CPU                   0
#define DARWIN_RLIMIT_FSIZE                 1
#define DARWIN_RLIMIT_DATA                  2
#define DARWIN_RLIMIT_STACK                 3
#define DARWIN_RLIMIT_CORE                  4
#define DARWIN_RLIMIT_AS                    5
#define DARWIN_RLIMIT_RSS                   5
#define DARWIN_RLIMIT_MEMLOCK               6
#define DARWIN_RLIMIT_NPROC                 7
#define DARWIN_RLIMIT_NOFILE                8
#define DARWIN_RLIM_INFINITY                0x7fffffffffffffff
#define DARWIN_RUSAGE_SELF                  0
#define DARWIN_RUSAGE_CHILDREN              -1
#define DARWIN_SIZEOF_RUSAGE                144
#define DARWIN_RUSAGE_ru_maxrss            32
#define DARWIN_SIZEOF_RLIMIT                16
#define DARWIN_SIZEOF_UTSNAME               1280
#define DARWIN_UTSNAME_nodename            256
#define DARWIN_UTSNAME_release             512
#define DARWIN_UTSNAME_version             768
#define DARWIN_UTSNAME_machine             1024

// time, locale, ctype
#define DARWIN_CLOCK_REALTIME               0
#define DARWIN_CLOCK_MONOTONIC              6
#define DARWIN_CLOCK_MONOTONIC_RAW          4
#define DARWIN_CLOCK_MONOTONIC_RAW_APPROX   5
#define DARWIN_CLOCK_UPTIME_RAW             8
#define DARWIN_CLOCK_UPTIME_RAW_APPROX      9
#define DARWIN_CLOCK_PROCESS_CPUTIME_ID     12
#define DARWIN_CLOCK_THREAD_CPUTIME_ID      16
#define DARWIN_SIZEOF_TM                    56
#define DARWIN_TM_tm_gmtoff                40
#define DARWIN_TM_tm_zone                  48
#define DARWIN_LC_ALL                       0
#define DARWIN_LC_COLLATE                   1
#define DARWIN_LC_CTYPE                     2
#define DARWIN_LC_MONETARY                  3
#define DARWIN_LC_NUMERIC                   4
#define DARWIN_LC_TIME                      5
#define DARWIN_LC_MESSAGES                  6
#define DARWIN_LC_COLLATE_MASK              0x1
#define DARWIN_LC_CTYPE_MASK                0x2
#define DARWIN_LC_MESSAGES_MASK             0x4
#define DARWIN_LC_MONETARY_MASK             0x8
#define DARWIN_LC_NUMERIC_MASK              0x10
#define DARWIN_LC_TIME_MASK                 0x20
#define DARWIN_LC_ALL_MASK                  0x3f
#define DARWIN_CODESET                      0
#define DARWIN_SIZEOF_RUNELOCALE            3208
#define DARWIN_RUNELOCALE___runetype       60
#define DARWIN_RUNELOCALE___maplower       1084
#define DARWIN_RUNELOCALE___mapupper       2108
#define DARWIN_RUNELOCALE___magic          0
#define DARWIN_RUNELOCALE___invalid_rune   56
#define DARWIN__CACHED_RUNES                256
#define DARWIN__CTYPE_A                     0x100
#define DARWIN__CTYPE_C                     0x200
#define DARWIN__CTYPE_D                     0x400
#define DARWIN__CTYPE_G                     0x800
#define DARWIN__CTYPE_L                     0x1000
#define DARWIN__CTYPE_P                     0x2000
#define DARWIN__CTYPE_S                     0x4000
#define DARWIN__CTYPE_U                     0x8000
#define DARWIN__CTYPE_X                     0x10000
#define DARWIN__CTYPE_B                     0x20000
#define DARWIN__CTYPE_R                     0x40000
#define DARWIN__CTYPE_I                     0x80000
#define DARWIN__CTYPE_T                     0x100000
#define DARWIN__CTYPE_Q                     0x200000
#define DARWIN_SIZEOF_LCONV                 96
#define DARWIN_SIZEOF_DL_INFO               32
#define DARWIN_SIZEOF_JMP_BUF               192
#define DARWIN_SIZEOF_SIGJMP_BUF            196
#define DARWIN_RTLD_LAZY                    0x1
#define DARWIN_RTLD_NOW                     0x2
#define DARWIN_RTLD_LOCAL                   0x4
#define DARWIN_RTLD_GLOBAL                  0x8
#define DARWIN_RTLD_NOLOAD                  0x10
#define DARWIN_RTLD_NODELETE                0x80
#define DARWIN_RTLD_FIRST                   0x100
#define DARWIN_RTLD_DEFAULT                 0xfffffffffffffffe
#define DARWIN_RTLD_NEXT                    0xffffffffffffffff
#define DARWIN_RTLD_SELF                    0xfffffffffffffffd
#define DARWIN_RTLD_MAIN_ONLY               0xfffffffffffffffb

// CoreFoundation
#define DARWIN_kCFStringEncodingUTF8        0x8000100
#define DARWIN_kCFStringEncodingASCII       0x600
#define DARWIN_kCFStringEncodingMacRoman    0x0
#define DARWIN_kCFStringEncodingUTF16       0x100
#define DARWIN_kCFStringEncodingUTF16LE     0x14000100
#define DARWIN_kCFStringEncodingUTF32       0xc000100
#define DARWIN_SIZEOF_CFRANGE               16
#define DARWIN_SIZEOF_CFINDEX               8

// xlocale, struct lconv
#define DARWIN_LC_GLOBAL_LOCALE             0xffffffffffffffff
#define DARWIN_LC_C_LOCALE                  0x0
#define DARWIN_LCONV_decimal_point         0
#define DARWIN_LCONV_thousands_sep         8
#define DARWIN_LCONV_grouping              16
#define DARWIN_LCONV_int_curr_symbol       24
#define DARWIN_LCONV_currency_symbol       32
#define DARWIN_LCONV_mon_decimal_point     40
#define DARWIN_LCONV_mon_thousands_sep     48
#define DARWIN_LCONV_mon_grouping          56
#define DARWIN_LCONV_positive_sign         64
#define DARWIN_LCONV_negative_sign         72
#define DARWIN_LCONV_int_frac_digits       80
#define DARWIN_LCONV_frac_digits           81
#define DARWIN_LCONV_p_cs_precedes         82
#define DARWIN_LCONV_p_sep_by_space        83
#define DARWIN_LCONV_n_cs_precedes         84
#define DARWIN_LCONV_n_sep_by_space        85
#define DARWIN_LCONV_p_sign_posn           86
#define DARWIN_LCONV_n_sign_posn           87
#define DARWIN_LCONV_int_p_cs_precedes     88
#define DARWIN_LCONV_int_n_cs_precedes     89
#define DARWIN_LCONV_int_p_sep_by_space    90
#define DARWIN_LCONV_int_n_sep_by_space    91
#define DARWIN_LCONV_int_p_sign_posn       92
#define DARWIN_LCONV_int_n_sign_posn       93
#define DARWIN_SIZEOF_MBSTATE_T             128
#define DARWIN_SIZEOF_WCHAR_T               4
#define DARWIN_SIZEOF_WINT_T                4
#define DARWIN_SIZEOF_LONG_DOUBLE           8
#define DARWIN_SIZEOF_TIMEZONE              8
#define DARWIN_SIZEOF_CLOCK_T               8
#define DARWIN_CLOCKS_PER_SEC               1000000
#define DARWIN_SIZEOF_DIV_T                 8
#define DARWIN_SIZEOF_FPOS_T                8
#define DARWIN_EOF                          -1
#define DARWIN_BUFSIZ                       1024
#define DARWIN__IOFBF                       0
#define DARWIN__IOLBF                       1
#define DARWIN__IONBF                       2

// signals
#define DARWIN_SIGHUP                       1
#define DARWIN_SIGINT                       2
#define DARWIN_SIGQUIT                      3
#define DARWIN_SIGILL                       4
#define DARWIN_SIGTRAP                      5
#define DARWIN_SIGABRT                      6
#define DARWIN_SIGEMT                       7
#define DARWIN_SIGFPE                       8
#define DARWIN_SIGKILL                      9
#define DARWIN_SIGBUS                       10
#define DARWIN_SIGSEGV                      11
#define DARWIN_SIGSYS                       12
#define DARWIN_SIGPIPE                      13
#define DARWIN_SIGALRM                      14
#define DARWIN_SIGTERM                      15
#define DARWIN_SIGURG                       16
#define DARWIN_SIGSTOP                      17
#define DARWIN_SIGTSTP                      18
#define DARWIN_SIGCONT                      19
#define DARWIN_SIGCHLD                      20
#define DARWIN_SIGTTIN                      21
#define DARWIN_SIGTTOU                      22
#define DARWIN_SIGIO                        23
#define DARWIN_SIGXCPU                      24
#define DARWIN_SIGXFSZ                      25
#define DARWIN_SIGVTALRM                    26
#define DARWIN_SIGPROF                      27
#define DARWIN_SIGWINCH                     28
#define DARWIN_SIGINFO                      29
#define DARWIN_SIGUSR1                      30
#define DARWIN_SIGUSR2                      31
#define DARWIN_NSIG                         32
#define DARWIN_SA_ONSTACK                   0x1
#define DARWIN_SA_RESTART                   0x2
#define DARWIN_SA_RESETHAND                 0x4
#define DARWIN_SA_NOCLDSTOP                 0x8
#define DARWIN_SA_NODEFER                   0x10
#define DARWIN_SA_NOCLDWAIT                 0x20
#define DARWIN_SA_SIGINFO                   0x40
#define DARWIN_SIG_BLOCK                    1
#define DARWIN_SIG_UNBLOCK                  2
#define DARWIN_SIG_SETMASK                  3
#define DARWIN_SIZEOF_SIGACTION             16
#define DARWIN_SIGACTION_sa_mask           8
#define DARWIN_SIGACTION_sa_flags          12
#define DARWIN_SIZEOF_SIGSET_T              4
#define DARWIN_SIZEOF_SIGINFO_T             104
#define DARWIN_SIGINFO_T_si_signo          0
#define DARWIN_SIGINFO_T_si_errno          4
#define DARWIN_SIGINFO_T_si_code           8
#define DARWIN_SIGINFO_T_si_addr           24
#define DARWIN_SIZEOF_STACK_T               24
#define DARWIN_STACK_T_ss_sp               0
#define DARWIN_STACK_T_ss_size             8
#define DARWIN_STACK_T_ss_flags            16

// pthread
#define DARWIN_SIZEOF_PTHREAD_MUTEX         64
#define DARWIN_SIZEOF_PTHREAD_COND          48
#define DARWIN_SIZEOF_PTHREAD_RWLOCK        200
#define DARWIN_SIZEOF_PTHREAD_ONCE          16
#define DARWIN_SIZEOF_PTHREAD_ATTR          64
#define DARWIN_SIZEOF_PTHREAD_MUTEXATTR     16
#define DARWIN_SIZEOF_PTHREAD_CONDATTR      16
#define DARWIN_SIZEOF_PTHREAD_RWLOCKATTR    24
#define DARWIN_SIZEOF_PTHREAD_KEY           8
#define DARWIN_SIZEOF_PTHREAD_T             8
#define DARWIN_PTHREAD_MUTEX_SIG_INIT       0x32aaaba7
#define DARWIN_PTHREAD_ERRORCHECK_SIG_INIT  0x32aaaba1
#define DARWIN_PTHREAD_RECURSIVE_SIG_INIT   0x32aaaba2
#define DARWIN_PTHREAD_COND_SIG_INIT        0x3cb0b1bb
#define DARWIN_PTHREAD_RWLOCK_SIG_INIT      0x2da8b3b4
#define DARWIN_PTHREAD_ONCE_SIG_INIT        0x30b1bcba
#define DARWIN_PTHREAD_MUTEX_NORMAL         0
#define DARWIN_PTHREAD_MUTEX_ERRORCHECK     1
#define DARWIN_PTHREAD_MUTEX_RECURSIVE      2
#define DARWIN_PTHREAD_MUTEX_DEFAULT        0
#define DARWIN_PTHREAD_CREATE_JOINABLE      1
#define DARWIN_PTHREAD_CREATE_DETACHED      2
#define DARWIN_PTHREAD_CANCEL_ENABLE        1
#define DARWIN_PTHREAD_CANCEL_DISABLE       0
#define DARWIN_PTHREAD_CANCEL_DEFERRED      2
#define DARWIN_PTHREAD_CANCEL_ASYNCHRONOUS  0
#define DARWIN_PTHREAD_SCOPE_SYSTEM         1
#define DARWIN_PTHREAD_SCOPE_PROCESS        2
#define DARWIN_PTHREAD_INHERIT_SCHED        1
#define DARWIN_PTHREAD_EXPLICIT_SCHED       2
#define DARWIN_PTHREAD_PROCESS_SHARED       1
#define DARWIN_PTHREAD_PROCESS_PRIVATE      2
#define DARWIN_PTHREAD_STACK_MIN            16384
#define DARWIN_SCHED_OTHER                  1
#define DARWIN_SCHED_FIFO                   4
#define DARWIN_SCHED_RR                     2
#define DARWIN_QOS_CLASS_USER_INTERACTIVE   33
#define DARWIN_QOS_CLASS_USER_INITIATED     25
#define DARWIN_QOS_CLASS_DEFAULT            21
#define DARWIN_QOS_CLASS_UTILITY            17
#define DARWIN_QOS_CLASS_BACKGROUND         9
#define DARWIN_QOS_CLASS_UNSPECIFIED        0

// Mach
#define DARWIN_KERN_SUCCESS                 0
#define DARWIN_KERN_INVALID_ARGUMENT        4
#define DARWIN_KERN_FAILURE                 5
#define DARWIN_ARM_THREAD_STATE64           6
#define DARWIN_ARM_THREAD_STATE64_COUNT     68
#define DARWIN_SIZEOF_ARM_THREAD_STATE64    272
#define DARWIN_ARM_THREAD_STATE64___x      0
#define DARWIN_ARM_THREAD_STATE64___fp     232
#define DARWIN_ARM_THREAD_STATE64___lr     240
#define DARWIN_ARM_THREAD_STATE64___sp     248
#define DARWIN_ARM_THREAD_STATE64___pc     256
#define DARWIN_ARM_THREAD_STATE64___cpsr   264
#define DARWIN_SIZEOF_MACH_TIMEBASE_INFO    8
#define DARWIN_SIZEOF_MACH_PORT_T           4
#define DARWIN_DISPATCH_TIME_NOW            0x0
#define DARWIN_DISPATCH_TIME_FOREVER        0xffffffffffffffff

// signal codes, the context a handler is given
#define DARWIN_ILL_NOOP                     0
#define DARWIN_ILL_ILLOPC                   1
#define DARWIN_ILL_ILLTRP                   2
#define DARWIN_ILL_PRVOPC                   3
#define DARWIN_ILL_ILLOPN                   4
#define DARWIN_ILL_ILLADR                   5
#define DARWIN_ILL_PRVREG                   6
#define DARWIN_ILL_COPROC                   7
#define DARWIN_ILL_BADSTK                   8
#define DARWIN_FPE_NOOP                     0
#define DARWIN_FPE_FLTDIV                   1
#define DARWIN_FPE_FLTOVF                   2
#define DARWIN_FPE_FLTUND                   3
#define DARWIN_FPE_FLTRES                   4
#define DARWIN_FPE_FLTINV                   5
#define DARWIN_FPE_FLTSUB                   6
#define DARWIN_FPE_INTDIV                   7
#define DARWIN_FPE_INTOVF                   8
#define DARWIN_SEGV_NOOP                    0
#define DARWIN_SEGV_MAPERR                  1
#define DARWIN_SEGV_ACCERR                  2
#define DARWIN_BUS_NOOP                     0
#define DARWIN_BUS_ADRALN                   1
#define DARWIN_BUS_ADRERR                   2
#define DARWIN_BUS_OBJERR                   3
#define DARWIN_TRAP_BRKPT                   1
#define DARWIN_TRAP_TRACE                   2
#define DARWIN_CLD_EXITED                   1
#define DARWIN_CLD_KILLED                   2
#define DARWIN_CLD_DUMPED                   3
#define DARWIN_CLD_TRAPPED                  4
#define DARWIN_CLD_STOPPED                  5
#define DARWIN_CLD_CONTINUED                6
#define DARWIN_SI_USER                      0x10001
#define DARWIN_SI_QUEUE                     0x10002
#define DARWIN_SI_TIMER                     0x10003
#define DARWIN_SI_ASYNCIO                   0x10004
#define DARWIN_SI_MESGQ                     0x10005
#define DARWIN_SIGINFO_T_si_pid            12
#define DARWIN_SIGINFO_T_si_uid            16
#define DARWIN_SIGINFO_T_si_status         20
#define DARWIN_SIGINFO_T_si_value          32
#define DARWIN_SIGINFO_T_si_band           40
#define DARWIN_SIG_DFL                      0x0
#define DARWIN_SIG_IGN                      0x1
#define DARWIN_SIG_HOLD                     0x5
#define DARWIN_SIG_ERR                      0xffffffffffffffff
#define DARWIN_SIZEOF_UCONTEXT              56
#define DARWIN_UCONTEXT_uc_onstack         0
#define DARWIN_UCONTEXT_uc_sigmask         4
#define DARWIN_UCONTEXT_uc_stack           8
#define DARWIN_UCONTEXT_uc_link            32
#define DARWIN_UCONTEXT_uc_mcsize          40
#define DARWIN_UCONTEXT_uc_mcontext        48
#define DARWIN_SIZEOF_MCONTEXT64            816
#define DARWIN_MCONTEXT64___es             0
#define DARWIN_MCONTEXT64___ss             16
#define DARWIN_MCONTEXT64___ns             288
#define DARWIN_SIZEOF_ARM_EXCEPTION_STATE64 16
#define DARWIN_ARM_EXCEPTION_STATE64___far 0
#define DARWIN_ARM_EXCEPTION_STATE64___esr 8
#define DARWIN_ARM_EXCEPTION_STATE64___exception 12
#define DARWIN_SIZEOF_ARM_NEON_STATE64      528
#define DARWIN_ARM_NEON_STATE64___v        0
#define DARWIN_ARM_NEON_STATE64___fpsr     512
#define DARWIN_ARM_NEON_STATE64___fpcr     516
#define DARWIN_SS_ONSTACK                   1
#define DARWIN_SS_DISABLE                   4
#define DARWIN_MINSIGSTKSZ                  32768
#define DARWIN_SIGSTKSZ                     131072

#endif
