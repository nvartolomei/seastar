/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2024 ScyllaDB
 */

#pragma once

// Compatibility shims for building Seastar on macOS (Darwin).
//
// macOS lacks a number of Linux-specific syscalls and ABI definitions that
// Seastar's POSIX layer and headers reference directly (epoll, eventfd,
// timerfd, CPU affinity sets, accept4, the linux/fs.h rename/ioctl flags,
// ...).  Rather than sprinkle `#ifdef __APPLE__` throughout the code base we
// centralize the declarations here.  The reactor uses a kqueue backend on
// macOS, so the epoll/timerfd/eventfd entry points exist only to let the
// shared headers compile; the ones that are actually exercised at runtime
// (CPU sets, accept4, pipe2, eventfd) are implemented in
// src/core/macos-compat.cc.
//
// On non-Apple platforms this header is empty.

#if defined(__APPLE__)

#include <cstdint>
#include <ctime>
#include <csignal>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// macOS spells the TCP keepalive idle time TCP_KEEPALIVE; Linux uses
// TCP_KEEPIDLE.  (TCP_KEEPCNT and TCP_KEEPINTVL exist on both.)
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

// POSIX.1-2008 names the struct stat timestamps st_Xtim; macOS predates that
// and spells them st_Xtimespec.
#include <sys/stat.h>
#ifndef st_mtim
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#endif

// Routing flags used when filtering routes.  RTF_POLICY/RTF_FLOW are Linux-only;
// defining them to 0 makes the corresponding checks no-ops on macOS.
#ifndef RTF_POLICY
#define RTF_POLICY 0
#endif
#ifndef RTF_FLOW
#define RTF_FLOW 0
#endif

// Linux uses SOL_IP as the setsockopt level for IP options; macOS (and the
// BSDs) use IPPROTO_IP.
#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

// ---------------------------------------------------------------------------
// Open/mmap/scheduler flags that macOS lacks.
//
// O_DIRECT: macOS has no O_DIRECT; unbuffered access is requested per-fd via
// fcntl(F_NOCACHE).  Defining it to 0 makes every "O_... | O_DIRECT" open fall
// back to buffered I/O, which is exactly the existing --kernel-page-cache code
// path (o_direct_flag = kernel_page_cache ? 0 : O_DIRECT).
// MAP_STACK is an advisory mmap hint with no macOS equivalent.
// SCHED_IDLE has no macOS analogue; fall back to the normal policy.
// ---------------------------------------------------------------------------

#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef MAP_STACK
#define MAP_STACK 0
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE SCHED_OTHER
#endif
// Transparent-hugepage madvise hints have no macOS equivalent; map them to
// MADV_NORMAL (0) so the advisory calls become no-ops.
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 0
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 0
#endif

// macOS's struct in6_addr only spells the byte accessor (s6_addr); Linux code
// also uses the 32-bit-word accessor s6_addr32.
#ifndef s6_addr32
#define s6_addr32 __u6_addr.__u6_addr32
#endif

extern "C" {

// ---------------------------------------------------------------------------
// epoll (only used to make the shared headers and the (unused on macOS) epoll
// backend parse; the kqueue backend is used at runtime).
// ---------------------------------------------------------------------------

enum {
    EPOLLIN = 0x001,
    EPOLLPRI = 0x002,
    EPOLLOUT = 0x004,
    EPOLLERR = 0x008,
    EPOLLHUP = 0x010,
    EPOLLRDHUP = 0x2000,
    EPOLLONESHOT = 1u << 30,
    EPOLLET = 1u << 31,
};

enum {
    EPOLL_CTL_ADD = 1,
    EPOLL_CTL_DEL = 2,
    EPOLL_CTL_MOD = 3,
};

#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC 02000000
#endif

typedef union epoll_data {
    void* ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
};

int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout,
                const sigset_t* sigmask);

// ---------------------------------------------------------------------------
// eventfd
// ---------------------------------------------------------------------------

enum {
    EFD_SEMAPHORE = 1,
    EFD_CLOEXEC = 02000000,
    EFD_NONBLOCK = 04000,
};

typedef uint64_t eventfd_t;

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t* value);
int eventfd_write(int fd, eventfd_t value);

// ---------------------------------------------------------------------------
// struct itimerspec — used by the POSIX timer / timerfd interfaces, which
// macOS lacks (it only provides struct itimerval).
// ---------------------------------------------------------------------------

struct itimerspec {
    struct timespec it_interval;  // timer period
    struct timespec it_value;     // timer expiration
};

// ---------------------------------------------------------------------------
// Real-time signals.  macOS has no SIGRTMIN/SIGRTMAX; Seastar only needs two
// distinct signals (the high-resolution timer and the stall detector), so we
// map them onto the user-defined signals.
// ---------------------------------------------------------------------------

#ifndef SIGRTMIN
#define SIGRTMIN SIGUSR1
#endif
#ifndef SIGRTMAX
#define SIGRTMAX SIGUSR2
#endif

// ---------------------------------------------------------------------------
// timerfd (declarations only; the kqueue backend uses EVFILT_TIMER instead)
// ---------------------------------------------------------------------------

#ifndef TFD_TIMER_ABSTIME
#define TFD_TIMER_ABSTIME (1 << 0)
#endif
#ifndef TFD_CLOEXEC
#define TFD_CLOEXEC 02000000
#endif
#ifndef TFD_NONBLOCK
#define TFD_NONBLOCK 04000
#endif

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec* new_value,
                    struct itimerspec* old_value);
int timerfd_gettime(int fd, struct itimerspec* curr_value);

// ---------------------------------------------------------------------------
// accept4 / pipe2 / socket type flags
// ---------------------------------------------------------------------------

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x0800
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x1000
#endif

int accept4(int sockfd, struct sockaddr* addr, socklen_t* addrlen, int flags);
int pipe2(int pipefd[2], int flags);

// ---------------------------------------------------------------------------
// linux/fs.h rename flags (renameat2).  macOS exposes a different set via
// renameatx_np, so these exist only so the rename_flags enum compiles; passing
// any of them to rename_file() is rejected at runtime.
// ---------------------------------------------------------------------------

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif
#ifndef RENAME_WHITEOUT
#define RENAME_WHITEOUT (1 << 2)
#endif

} // extern "C"

// ---------------------------------------------------------------------------
// CPU affinity sets.  macOS has no notion of pinning a thread to a core, so
// the cpu_set_t type exists for bookkeeping (resource detection) and the
// pthread_*affinity_np entry points are no-ops.
// ---------------------------------------------------------------------------

#ifndef CPU_SETSIZE
#define CPU_SETSIZE 1024
#endif

#define __SEASTAR_CPU_SET_NWORDS (CPU_SETSIZE / (8 * sizeof(uint64_t)))

typedef struct cpu_set_t {
    uint64_t __bits[__SEASTAR_CPU_SET_NWORDS];
} cpu_set_t;

static inline void CPU_ZERO(cpu_set_t* set) {
    for (unsigned __i = 0; __i < __SEASTAR_CPU_SET_NWORDS; ++__i) {
        set->__bits[__i] = 0;
    }
}

static inline void CPU_SET(int cpu, cpu_set_t* set) {
    set->__bits[cpu / 64] |= (uint64_t(1) << (cpu % 64));
}

static inline void CPU_CLR(int cpu, cpu_set_t* set) {
    set->__bits[cpu / 64] &= ~(uint64_t(1) << (cpu % 64));
}

static inline int CPU_ISSET(int cpu, const cpu_set_t* set) {
    return (set->__bits[cpu / 64] & (uint64_t(1) << (cpu % 64))) != 0;
}

static inline int CPU_COUNT(const cpu_set_t* set) {
    int __n = 0;
    for (unsigned __i = 0; __i < __SEASTAR_CPU_SET_NWORDS; ++__i) {
        __n += __builtin_popcountll(set->__bits[__i]);
    }
    return __n;
}

extern "C" {
int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t* cpuset);
int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t* cpuset);
}

// macOS's pthread_setname_np takes only the thread name and always applies to
// the calling thread.  Seastar always names the current thread (it passes
// pthread_self()), so rewrite the 2-argument Linux form to the 1-argument
// macOS form.  The rewritten call has a single argument and therefore does not
// re-trigger this function-like macro.
#define pthread_setname_np(thread, name) pthread_setname_np(name)

// ---------------------------------------------------------------------------
// getpagesize() is hidden by the macOS SDK when _XOPEN_SOURCE is requested;
// provide it in terms of the always-available sysconf().
// ---------------------------------------------------------------------------

#include <unistd.h>

static inline int getpagesize() {
    return static_cast<int>(::sysconf(_SC_PAGESIZE));
}

// ---------------------------------------------------------------------------
// getrusage who-argument.  macOS has no per-thread accounting; fall back to
// per-process so the (rarely used) disk benchmark still links.
// ---------------------------------------------------------------------------

#include <sys/resource.h>
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD RUSAGE_SELF
#endif

// ---------------------------------------------------------------------------
// inotify (file-change notifications).  macOS has no inotify; the entry points
// are stubbed (see macos-compat.cc) and return ENOSYS, so file watching is a
// no-op rather than a build failure.
// ---------------------------------------------------------------------------

#include <fcntl.h>

#ifndef IN_NONBLOCK
#define IN_NONBLOCK O_NONBLOCK
#endif
#ifndef IN_CLOEXEC
#define IN_CLOEXEC O_CLOEXEC
#endif

enum {
    IN_ACCESS = 0x00000001,
    IN_MODIFY = 0x00000002,
    IN_ATTRIB = 0x00000004,
    IN_CLOSE_WRITE = 0x00000008,
    IN_CLOSE_NOWRITE = 0x00000010,
    IN_OPEN = 0x00000020,
    IN_MOVED_FROM = 0x00000040,
    IN_MOVED_TO = 0x00000080,
    IN_CREATE = 0x00000100,
    IN_DELETE = 0x00000200,
    IN_DELETE_SELF = 0x00000400,
    IN_MOVE_SELF = 0x00000800,
    IN_IGNORED = 0x00008000,
    IN_CLOSE = IN_CLOSE_WRITE | IN_CLOSE_NOWRITE,
    IN_MOVE = IN_MOVED_FROM | IN_MOVED_TO,
};

// Watch-control flags (these have bit 31 set, hence kept out of the enum above).
#ifndef IN_ONLYDIR
#define IN_ONLYDIR 0x01000000
#endif
#ifndef IN_ONESHOT
#define IN_ONESHOT 0x80000000
#endif

struct inotify_event {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[];
};

extern "C" {
int inotify_init();
int inotify_init1(int flags);
int inotify_add_watch(int fd, const char* pathname, uint32_t mask);
int inotify_rm_watch(int fd, int wd);
}

// ---------------------------------------------------------------------------
// dl_iterate_phdr (shared-object enumeration for backtraces).  macOS has no
// <link.h>; we provide an ELF-shaped view backed by dyld (see macos-compat.cc).
// ---------------------------------------------------------------------------

#ifndef PT_LOAD
#define PT_LOAD 1
#endif

// Minimal stand-in for ElfW(Phdr): backtrace.cc only reads p_type and p_memsz.
struct seastar_macos_elf_phdr {
    uint32_t p_type;
    uint64_t p_memsz;
};

struct dl_phdr_info {
    uintptr_t dlpi_addr;
    const char* dlpi_name;
    const struct seastar_macos_elf_phdr* dlpi_phdr;
    uint16_t dlpi_phnum;
};

extern "C" int dl_iterate_phdr(
    int (*callback)(struct dl_phdr_info* info, size_t size, void* data),
    void* data);

// ---------------------------------------------------------------------------
// linux/fs.h block-device ioctls and fallocate flags, plus fallocate()/
// fdatasync() which macOS lacks (see macos-compat.cc).  The block ioctls are
// only issued against block devices and fail with ENOTSUP on macOS.
// ---------------------------------------------------------------------------

#ifndef BLKSSZGET
#define BLKSSZGET 0x1268
#endif
#ifndef BLKPBSZGET
#define BLKPBSZGET 0x127b
#endif
#ifndef BLKBSZGET
#define BLKBSZGET 0x80081270
#endif
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 0x80081272
#endif
#ifndef BLKDISCARD
#define BLKDISCARD 0x1277
#endif

// linux/major.h: the md (software RAID) block-device major number; no macOS
// block device will match it, but the constant is referenced unconditionally.
#ifndef MD_MAJOR
#define MD_MAJOR 9
#endif

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#endif

extern "C" {
int fallocate(int fd, int mode, off_t offset, off_t len);
int fdatasync(int fd);
}

// fcntl write-life-time hints (Linux-only); macOS has no equivalent, so the
// fcntl() call simply fails (the hints are advisory).
#ifndef F_GET_RW_HINT
#define F_GET_RW_HINT 1030
#endif
#ifndef F_SET_RW_HINT
#define F_SET_RW_HINT 1031
#endif

#endif // defined(__APPLE__)
