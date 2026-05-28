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

// Implementations of the Linux syscalls that Seastar's POSIX layer references
// but which macOS lacks.  The declarations live in
// <seastar/util/macos-compat.hh>.  Functions that have no meaningful macOS
// analogue (epoll, timerfd, inotify) are stubbed to fail with ENOSYS; the
// reactor uses a kqueue backend and does not rely on them.

#if defined(__APPLE__)

#include <seastar/util/macos-compat.hh>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern "C" {

// --- eventfd ----------------------------------------------------------------
//
// macOS has no eventfd.  Seastar relies on three eventfd properties: a single
// descriptor on which ::write() loops back to a subsequent ::read(); the
// descriptor being pollable for readiness; and dup() producing a second handle
// that shares the same object (readable_eventfd/writeable_eventfd::*_side()).
//
// A self-connected loopback UDP socket provides all three: a datagram written
// to the socket is delivered to its own receive buffer (so write→read loops
// back), the socket is pollable via kqueue, and dup() shares the socket.  The
// 8-byte payloads are exchanged verbatim; unlike a real eventfd the counter is
// not coalesced, but Seastar only uses these descriptors for wakeup/handshake,
// where a datagram per signal is equivalent.

int eventfd(unsigned int initval, int flags) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return -1;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t alen = sizeof(addr);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &alen) < 0 ||
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), alen) < 0) {
        ::close(s);
        return -1;
    }
    if (flags & EFD_NONBLOCK) {
        int fl = ::fcntl(s, F_GETFL, 0);
        ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
    }
    if (flags & EFD_CLOEXEC) {
        ::fcntl(s, F_SETFD, FD_CLOEXEC);
    }
    if (initval) {
        eventfd_t v = initval;
        (void)::send(s, &v, sizeof(v), 0);
    }
    return s;
}

int eventfd_read(int fd, eventfd_t* value) {
    return ::read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value) ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value) {
    return ::write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value) ? 0 : -1;
}

// --- accept4 / pipe2 --------------------------------------------------------

int accept4(int sockfd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
    int fd = ::accept(sockfd, addr, addrlen);
    if (fd < 0) {
        return fd;
    }
    if (flags & SOCK_NONBLOCK) {
        int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl == -1 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1) {
            ::close(fd);
            return -1;
        }
    }
    if (flags & SOCK_CLOEXEC) {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
            ::close(fd);
            return -1;
        }
    }
    return fd;
}

int pipe2(int pipefd[2], int flags) {
    if (::pipe(pipefd) != 0) {
        return -1;
    }
    for (int i = 0; i < 2; ++i) {
        if (flags & O_NONBLOCK) {
            int fl = ::fcntl(pipefd[i], F_GETFL, 0);
            if (fl == -1 || ::fcntl(pipefd[i], F_SETFL, fl | O_NONBLOCK) == -1) {
                goto fail;
            }
        }
        if (flags & O_CLOEXEC) {
            if (::fcntl(pipefd[i], F_SETFD, FD_CLOEXEC) == -1) {
                goto fail;
            }
        }
    }
    return 0;
fail:
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return -1;
}

// --- epoll (unused on macOS; the kqueue backend is used instead) ------------

int epoll_create1(int) { errno = ENOSYS; return -1; }
int epoll_ctl(int, int, int, struct epoll_event*) { errno = ENOSYS; return -1; }
int epoll_wait(int, struct epoll_event*, int, int) { errno = ENOSYS; return -1; }
int epoll_pwait(int, struct epoll_event*, int, int, const sigset_t*) { errno = ENOSYS; return -1; }

// --- timerfd (unused on macOS; the kqueue backend uses EVFILT_TIMER) --------

int timerfd_create(int, int) { errno = ENOSYS; return -1; }
int timerfd_settime(int, int, const struct itimerspec*, struct itimerspec*) { errno = ENOSYS; return -1; }
int timerfd_gettime(int, struct itimerspec*) { errno = ENOSYS; return -1; }

// --- inotify (unused on macOS; file watching is unsupported) ----------------

int inotify_init() { errno = ENOSYS; return -1; }
int inotify_init1(int) { errno = ENOSYS; return -1; }
int inotify_add_watch(int, const char*, uint32_t) { errno = ENOSYS; return -1; }
int inotify_rm_watch(int, int) { errno = ENOSYS; return -1; }

// --- CPU affinity (macOS cannot pin threads; treated as a no-op) ------------
//
// setaffinity is a no-op.  getaffinity must report the set of CPUs the thread
// may run on; macOS has no per-thread affinity, so report all online CPUs.
// (This is load-bearing: get_current_cpuset() counts the returned set to size
// the default shard count, so an unfilled set would yield a garbage count.)

int pthread_setaffinity_np(pthread_t, size_t, const cpu_set_t*) { return 0; }

int pthread_getaffinity_np(pthread_t, size_t, cpu_set_t* cpuset) {
    if (cpuset) {
        CPU_ZERO(cpuset);
        long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        for (long i = 0; i < n && i < CPU_SETSIZE; ++i) {
            CPU_SET(static_cast<int>(i), cpuset);
        }
    }
    return 0;
}

// --- fallocate / fdatasync --------------------------------------------------

int fallocate(int fd, int mode, off_t offset, off_t len) {
    if (mode & FALLOC_FL_PUNCH_HOLE) {
        struct fpunchhole hole = {};
        hole.fp_offset = offset;
        hole.fp_length = len;
        if (::fcntl(fd, F_PUNCHHOLE, &hole) == -1) {
            return -1;
        }
        return 0;
    }
    if (mode == 0) {
        // Best-effort contiguous preallocation followed by a size extension.
        fstore_t store = {};
        store.fst_flags = F_ALLOCATECONTIG;
        store.fst_posmode = F_PEOFPOSMODE;
        store.fst_offset = 0;
        store.fst_length = offset + len;
        if (::fcntl(fd, F_PREALLOCATE, &store) == -1) {
            store.fst_flags = F_ALLOCATEALL;
            if (::fcntl(fd, F_PREALLOCATE, &store) == -1) {
                return -1;
            }
        }
        return ::ftruncate(fd, offset + len);
    }
    errno = ENOTSUP;
    return -1;
}

int fdatasync(int fd) {
    // macOS has no fdatasync; fsync provides (stronger) durability.
    return ::fsync(fd);
}

// --- dl_iterate_phdr via dyld ----------------------------------------------
//
// Present each loaded Mach-O image as a single PT_LOAD "segment" whose
// [dlpi_addr, dlpi_addr + p_memsz) range covers the image's mapped address
// space, which is all backtrace.cc needs to attribute an address to an object.

int dl_iterate_phdr(
        int (*callback)(struct dl_phdr_info* info, size_t size, void* data),
        void* data) {
    const uint32_t count = _dyld_image_count();
    int ret = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mach_header* mh = _dyld_get_image_header(i);
        if (!mh) {
            continue;
        }
        const intptr_t slide = _dyld_get_image_vmaddr_slide(i);

        // Walk load commands, computing the image's vmaddr span.
        uint64_t min_vmaddr = UINT64_MAX;
        uint64_t max_vmend = 0;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(mh);
        bool is64 = (mh->magic == MH_MAGIC_64 || mh->magic == MH_CIGAM_64);
        p += is64 ? sizeof(struct mach_header_64) : sizeof(struct mach_header);
        const struct load_command* lc = reinterpret_cast<const struct load_command*>(p);
        for (uint32_t c = 0; c < mh->ncmds; ++c) {
            if (lc->cmd == LC_SEGMENT_64) {
                auto seg = reinterpret_cast<const struct segment_command_64*>(lc);
                if (std::strcmp(seg->segname, "__PAGEZERO") != 0 && seg->vmsize) {
                    min_vmaddr = seg->vmaddr < min_vmaddr ? seg->vmaddr : min_vmaddr;
                    uint64_t end = seg->vmaddr + seg->vmsize;
                    max_vmend = end > max_vmend ? end : max_vmend;
                }
            } else if (lc->cmd == LC_SEGMENT) {
                auto seg = reinterpret_cast<const struct segment_command*>(lc);
                if (std::strcmp(seg->segname, "__PAGEZERO") != 0 && seg->vmsize) {
                    min_vmaddr = seg->vmaddr < min_vmaddr ? seg->vmaddr : min_vmaddr;
                    uint64_t end = seg->vmaddr + seg->vmsize;
                    max_vmend = end > max_vmend ? end : max_vmend;
                }
            }
            lc = reinterpret_cast<const struct load_command*>(
                    reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
        }
        if (min_vmaddr == UINT64_MAX) {
            continue;
        }

        struct seastar_macos_elf_phdr phdr;
        phdr.p_type = PT_LOAD;
        phdr.p_memsz = max_vmend - min_vmaddr;

        struct dl_phdr_info info;
        info.dlpi_addr = static_cast<uintptr_t>(slide + min_vmaddr);
        info.dlpi_name = _dyld_get_image_name(i);
        info.dlpi_phdr = &phdr;
        info.dlpi_phnum = 1;

        ret = callback(&info, sizeof(info), data);
        if (ret != 0) {
            break;
        }
    }
    return ret;
}

} // extern "C"

#endif // defined(__APPLE__)
