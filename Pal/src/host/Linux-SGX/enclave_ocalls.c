/*
 * This is for enclave to make ocalls to untrusted runtime.
 */

#include "ecall_types.h"
#include "enclave_ocalls.h"
#include "ocall_types.h"
#include "pal_debug.h"
#include "pal_internal.h"
#include "pal_linux.h"
#include "pal_linux_error.h"
#include "rpcqueue.h"
#include "spinlock.h"
#include <api.h>
#include <asm/errno.h>
#include <linux/futex.h>
#include <stdalign.h>

/* Check against this limit if the buffer to be allocated fits on the untrusted stack; if not,
 * buffer will be allocated on untrusted heap. Conservatively set this limit to 1/4 of the
 * actual stack size. Currently THREAD_STACK_SIZE = 2MB, so this limit is 512KB.
 * Note that the main thread is special in that it is handled by Linux, with the typical stack
 * size of 8MB. Thus, 512KB limit also works well for the main thread. */
#define MAX_UNTRUSTED_STACK_BUF (THREAD_STACK_SIZE / 4)

/* global pointer to a single untrusted queue, requires proper synchronization */
rpc_queue_t* g_rpc_queue;

static int sgx_exitless_ocall(int code, void* ms) {
    /* perform OCALL with enclave exit if no RPC queue (i.e., no exitless); no need for atomics
     * because this pointer is set only once at enclave initialization */
    if (!g_rpc_queue)
        return sgx_ocall(code, ms);

    /* allocate request on OCALL stack; it is automatically freed on OCALL end; note that request's
     * lock is used in futex() and must be aligned to 4B */
    rpc_request_t* req = sgx_alloc_on_ustack_aligned(sizeof(*req), alignof(*req));
    req->ocall_index = code;
    req->buffer      = ms;
    spinlock_init(&req->lock);

    /* grab the lock on this request (it is the responsibility of RPC thread to unlock it when
     * done); this always succeeds immediately since enclave thread is currently the only owner
     * of the lock */
    spinlock_lock(&req->lock);

    /* enqueue OCALL request into RPC queue; some RPC thread will dequeue it, issue a syscall
     * and, after syscall is finished, release the request's spinlock */
    req = rpc_enqueue(g_rpc_queue, req);
    if (!req) {
        /* no space in queue: all RPC threads are busy with outstanding ocalls;
         * fallback to normal syscall path with enclave exit */
        return sgx_ocall(code, ms);
    }

    /* wait till request processing is finished; try spinlock first */
    int timedout = spinlock_lock_timeout(&req->lock, RPC_SPINLOCK_TIMEOUT);

    /* at this point:
     * - either RPC thread is done with OCALL and released the request's spinlock,
     *   and our enclave thread grabbed lock but it doesn't matter at this point
     *   (OCALL is done, timedout = 0, no need to wait on futex)
     * - or OCALL is still pending and the request is still blocked on spinlock
     *   (OCALL is not done, timedout != 0, let's wait on futex) */

    if (timedout) {
        /* OCALL takes a lot of time, so fallback to waiting on a futex; at this point we exit
         * enclave to perform syscall; this code is based on Mutex 2 from Futexes are Tricky */
        int c = SPINLOCK_UNLOCKED;

        /* at this point can be a subtle data race: RPC thread is only now done with OCALL and
         * moved lock in UNLOCKED state; in this racey case, lock = UNLOCKED = 0 and we do not
         * wait on futex (note that enclave thread grabbed lock but it doesn't matter) */
        if (!spinlock_cmpxchg(&req->lock, &c, SPINLOCK_LOCKED_NO_WAITERS)) {
            /* allocate futex args on OCALL stack; automatically freed on OCALL end */
            ms_ocall_futex_t* ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
            ms->ms_futex = (int*)&req->lock;
            ms->ms_op = FUTEX_WAIT_PRIVATE;
            ms->ms_timeout_us = -1; /* never time out */

            do {
                /* at this point lock = LOCKED_*; before waiting on futex, need to move lock to
                 * LOCKED_WITH_WAITERS; note that check on cmpxchg of lock = UNLOCKED = 0 is for
                 * the same data race as above */
                if (c == SPINLOCK_LOCKED_WITH_WAITERS || /* shortcut: don't need to move lock state */
                    spinlock_cmpxchg(&req->lock, &c, SPINLOCK_LOCKED_WITH_WAITERS)) {
                    /* at this point, futex(wait) syscall expects lock to be in LOCKED_WITH_WAITERS
                     * set by enclave thread above; if RPC thread moved it back to UNLOCKED, futex()
                     * immediately returns */
                    ms->ms_val = SPINLOCK_LOCKED_WITH_WAITERS;
                    int ret = sgx_ocall(OCALL_FUTEX, ms);
                    if (ret < 0 && ret != -EAGAIN)
                        return -EPERM;
                }
                c = SPINLOCK_UNLOCKED;
            } while (!spinlock_cmpxchg(&req->lock, &c, SPINLOCK_LOCKED_WITH_WAITERS));
            /* while-loop is required for spurious futex wake-ups: our enclave thread must wait
             * until lock moves to UNLOCKED (note that enclave thread grabs lock but it doesn't
             * matter at this point) */
        }
    }

    return req->result;
}

noreturn void ocall_exit(int exitcode, int is_exitgroup)
{
    ms_ocall_exit_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    ms->ms_exitcode     = exitcode;
    ms->ms_is_exitgroup = is_exitgroup;

    // There are two reasons for this loop:
    //  1. Ocalls can be interuppted.
    //  2. We can't trust the outside to actually exit, so we need to ensure
    //     that we never return even when the outside tries to trick us.
    while (true) {
        sgx_ocall(OCALL_EXIT, ms);
    }
}

int ocall_mmap_untrusted (int fd, uint64_t offset,
                          uint64_t size, unsigned short prot,
                          void ** mem)
{
    int retval = 0;
    ms_ocall_mmap_untrusted_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;
    ms->ms_offset = offset;
    ms->ms_size = size;
    ms->ms_prot = prot;

    retval = sgx_exitless_ocall(OCALL_MMAP_UNTRUSTED, ms);

    if (!retval) {
        if (!sgx_copy_ptr_to_enclave(mem, ms->ms_mem, size)) {
            sgx_reset_ustack();
            return -EPERM;
        }
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_munmap_untrusted (const void * mem, uint64_t size)
{
    int retval = 0;
    ms_ocall_munmap_untrusted_t * ms;

    if (!sgx_is_completely_outside_enclave(mem, size)) {
        sgx_reset_ustack();
        return -EINVAL;
    }

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_mem  = mem;
    ms->ms_size = size;

    retval = sgx_exitless_ocall(OCALL_MUNMAP_UNTRUSTED, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_cpuid (unsigned int leaf, unsigned int subleaf,
                 unsigned int values[4])
{
    int retval = 0;
    ms_ocall_cpuid_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_leaf = leaf;
    ms->ms_subleaf = subleaf;

    retval = sgx_exitless_ocall(OCALL_CPUID, ms);

    if (!retval) {
        values[0] = ms->ms_values[0];
        values[1] = ms->ms_values[1];
        values[2] = ms->ms_values[2];
        values[3] = ms->ms_values[3];
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_open (const char * pathname, int flags, unsigned short mode)
{
    int retval = 0;
    int len = pathname ? strlen(pathname) + 1 : 0;
    ms_ocall_open_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_flags = flags;
    ms->ms_mode = mode;
    ms->ms_pathname = sgx_copy_to_ustack(pathname, len);

    if (!ms->ms_pathname) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_OPEN, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_close (int fd)
{
    int retval = 0;
    ms_ocall_close_t *ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;

    retval = sgx_exitless_ocall(OCALL_CLOSE, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_read (int fd, void * buf, unsigned int count)
{
    int retval = 0;
    void * obuf = NULL;
    ms_ocall_read_t * ms;

    if (count > MAX_UNTRUSTED_STACK_BUF) {
        retval = ocall_mmap_untrusted(-1, 0, ALLOC_ALIGNUP(count), PROT_READ | PROT_WRITE, &obuf);
        if (IS_ERR(retval))
            return retval;
    }

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        retval = -EPERM;
        goto out;
    }

    ms->ms_fd = fd;
    ms->ms_count = count;
    if (obuf)
        ms->ms_buf = obuf;
    else
        ms->ms_buf = sgx_alloc_on_ustack(count);

    if (!ms->ms_buf) {
        retval = -EPERM;
        goto out;
    }

    retval = sgx_exitless_ocall(OCALL_READ, ms);

    if (retval > 0) {
        if (!sgx_copy_to_enclave(buf, count, ms->ms_buf, retval)) {
            retval = -EPERM;
            goto out;
        }
    }

out:
    sgx_reset_ustack();
    if (obuf)
        ocall_munmap_untrusted(obuf, ALLOC_ALIGNUP(count));
    return retval;
}

int ocall_write (int fd, const void * buf, unsigned int count)
{
    int retval = 0;
    void * obuf = NULL;
    ms_ocall_write_t * ms;

    if (sgx_is_completely_outside_enclave(buf, count)) {
        /* buf is in untrusted memory (e.g., allowed file mmaped in untrusted memory) */
        obuf = (void*)buf;
    } else if (sgx_is_completely_within_enclave(buf, count)) {
        /* typical case of buf inside of enclave memory */
        if (count > MAX_UNTRUSTED_STACK_BUF) {
            /* buf is too big and may overflow untrusted stack, so use untrusted heap */
            retval = ocall_mmap_untrusted(-1, 0, ALLOC_ALIGNUP(count), PROT_READ | PROT_WRITE, &obuf);
            if (IS_ERR(retval))
                return retval;
            memcpy(obuf, buf, count);
        }
    } else {
        /* buf is partially in/out of enclave memory */
        return -EPERM;
    }

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        retval = -EPERM;
        goto out;
    }

    ms->ms_fd = fd;
    ms->ms_count = count;
    if (obuf)
        ms->ms_buf = obuf;
    else
        ms->ms_buf = sgx_copy_to_ustack(buf, count);

    if (!ms->ms_buf) {
        retval = -EPERM;
        goto out;
    }

    retval = sgx_exitless_ocall(OCALL_WRITE, ms);

out:
    sgx_reset_ustack();
    if (obuf && obuf != buf)
        ocall_munmap_untrusted(obuf, ALLOC_ALIGNUP(count));
    return retval;
}

int ocall_fstat (int fd, struct stat * buf)
{
    int retval = 0;
    ms_ocall_fstat_t * ms;


    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;

    retval = sgx_exitless_ocall(OCALL_FSTAT, ms);

    if (!retval)
        memcpy(buf, &ms->ms_stat, sizeof(struct stat));

    sgx_reset_ustack();
    return retval;
}

int ocall_fionread (int fd)
{
    int retval = 0;
    ms_ocall_fionread_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;

    retval = sgx_exitless_ocall(OCALL_FIONREAD, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_fsetnonblock (int fd, int nonblocking)
{
    int retval = 0;
    ms_ocall_fsetnonblock_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;
    ms->ms_nonblocking = nonblocking;

    retval = sgx_exitless_ocall(OCALL_FSETNONBLOCK, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_fchmod (int fd, unsigned short mode)
{
    int retval = 0;
    ms_ocall_fchmod_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;
    ms->ms_mode = mode;

    retval = sgx_exitless_ocall(OCALL_FCHMOD, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_fsync (int fd)
{
    int retval = 0;
    ms_ocall_fsync_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;

    retval = sgx_exitless_ocall(OCALL_FSYNC, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_ftruncate (int fd, uint64_t length)
{
    int retval = 0;
    ms_ocall_ftruncate_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;
    ms->ms_length = length;

    retval = sgx_exitless_ocall(OCALL_FTRUNCATE, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_lseek(int fd, uint64_t offset, int whence) {
    int retval = 0;
    ms_ocall_lseek_t* ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd     = fd;
    ms->ms_offset = offset;
    ms->ms_whence = whence;

    retval = sgx_exitless_ocall(OCALL_LSEEK, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_mkdir (const char * pathname, unsigned short mode)
{
    int retval = 0;
    int len = pathname ? strlen(pathname) + 1 : 0;
    ms_ocall_mkdir_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_mode = mode;
    ms->ms_pathname = sgx_copy_to_ustack(pathname, len);

    if (!ms->ms_pathname) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_MKDIR, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_getdents (int fd, struct linux_dirent64 * dirp, unsigned int size)
{
    int retval = 0;
    ms_ocall_getdents_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_fd = fd;
    ms->ms_size = size;
    ms->ms_dirp = sgx_alloc_on_ustack(size);

    if (!ms->ms_dirp) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_GETDENTS, ms);

    if (retval > 0) {
        if (!sgx_copy_to_enclave(dirp, size, ms->ms_dirp, retval)) {
            sgx_reset_ustack();
            return -EPERM;
        }
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_resume_thread (void * tcs)
{
    return sgx_exitless_ocall(OCALL_RESUME_THREAD, tcs);
}

int ocall_clone_thread (void)
{
    void* dummy = NULL;
    return sgx_exitless_ocall(OCALL_CLONE_THREAD, dummy);
}

int ocall_create_process(const char* uri, int nargs, const char** args, int procfds[3],
                         unsigned int* pid) {
    int retval = 0;
    int ulen = uri ? strlen(uri) + 1 : 0;
    ms_ocall_create_process_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms) + nargs * sizeof(char*), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_uri = uri ? sgx_copy_to_ustack(uri, ulen) : NULL;
    if (uri && !ms->ms_uri) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_nargs = nargs;
    for (int i = 0 ; i < nargs ; i++) {
        int len = args[i] ? strlen(args[i]) + 1 : 0;
        ms->ms_args[i] = args[i] ? sgx_copy_to_ustack(args[i], len) : NULL;

        if (args[i] && !ms->ms_args[i]) {
            sgx_reset_ustack();
            return -EPERM;
        }
    }

    retval = sgx_exitless_ocall(OCALL_CREATE_PROCESS, ms);

    if (!retval) {
        if (pid)
            *pid = ms->ms_pid;
        procfds[0] = ms->ms_proc_fds[0];
        procfds[1] = ms->ms_proc_fds[1];
        procfds[2] = ms->ms_proc_fds[2];
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_futex(int* futex, int op, int val, int64_t timeout_us) {
    int retval = 0;
    ms_ocall_futex_t * ms;

    if (!sgx_is_completely_outside_enclave(futex, sizeof(int))) {
        sgx_reset_ustack();
        return -EINVAL;
    }

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_futex = futex;
    ms->ms_op = op;
    ms->ms_val = val;
    ms->ms_timeout_us = timeout_us;

    retval = sgx_exitless_ocall(OCALL_FUTEX, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_socketpair (int domain, int type, int protocol,
                      int sockfds[2])
{
    int retval = 0;
    ms_ocall_socketpair_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_domain = domain;
    ms->ms_type = type;
    ms->ms_protocol = protocol;

    retval = sgx_exitless_ocall(OCALL_SOCKETPAIR, ms);

    if (!retval) {
        sockfds[0] = ms->ms_sockfds[0];
        sockfds[1] = ms->ms_sockfds[1];
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_listen (int domain, int type, int protocol,
                  struct sockaddr * addr, unsigned int * addrlen,
                  struct sockopt * sockopt)
{
    int retval = 0;
    unsigned int copied;
    unsigned int len = addrlen ? *addrlen : 0;
    ms_ocall_listen_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_domain = domain;
    ms->ms_type = type;
    ms->ms_protocol = protocol;
    ms->ms_addrlen = len;
    ms->ms_addr = (addr && len) ? sgx_copy_to_ustack(addr, len) : NULL;

    if (addr && len && !ms->ms_addr) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_LISTEN, ms);

    if (retval >= 0) {
        if (addr && len) {
            copied = sgx_copy_to_enclave(addr, len, ms->ms_addr, ms->ms_addrlen);
            if (!copied) {
                sgx_reset_ustack();
                return -EPERM;
            }
            *addrlen = copied;
        }

        if (sockopt) {
            *sockopt = ms->ms_sockopt;
        }
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_accept (int sockfd, struct sockaddr * addr,
                  unsigned int * addrlen, struct sockopt * sockopt)
{
    int retval = 0;
    unsigned int copied;
    unsigned int len = addrlen ? *addrlen : 0;
    ms_ocall_accept_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_sockfd = sockfd;
    ms->ms_addrlen = len;
    ms->ms_addr = (addr && len) ? sgx_copy_to_ustack(addr, len) : NULL;

    if (addr && len && !ms->ms_addr) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_ACCEPT, ms);

    if (retval >= 0) {
        if (addr && len) {
            copied = sgx_copy_to_enclave(addr, len, ms->ms_addr, ms->ms_addrlen);
            if (!copied) {
                sgx_reset_ustack();
                return -EPERM;
            }
            *addrlen = copied;
        }

        if (sockopt) {
            *sockopt = ms->ms_sockopt;
        }
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_connect (int domain, int type, int protocol,
                   const struct sockaddr * addr,
                   unsigned int addrlen,
                   struct sockaddr * bind_addr,
                   unsigned int * bind_addrlen, struct sockopt * sockopt)
{
    int retval = 0;
    unsigned int copied;
    unsigned int bind_len = bind_addrlen ? *bind_addrlen : 0;
    ms_ocall_connect_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_domain = domain;
    ms->ms_type = type;
    ms->ms_protocol = protocol;
    ms->ms_addrlen = addrlen;
    ms->ms_bind_addrlen = bind_len;
    ms->ms_addr = addr ? sgx_copy_to_ustack(addr, addrlen) : NULL;
    ms->ms_bind_addr = bind_addr ? sgx_copy_to_ustack(bind_addr, bind_len) : NULL;

    if ((addr && !ms->ms_addr) || (bind_addr && !ms->ms_bind_addr)) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_CONNECT, ms);

    if (retval >= 0) {
        if (bind_addr && bind_len) {
            copied = sgx_copy_to_enclave(bind_addr, bind_len, ms->ms_bind_addr, ms->ms_bind_addrlen);
            if (!copied) {
                sgx_reset_ustack();
                return -EPERM;
            }
            *bind_addrlen = copied;
        }

        if (sockopt) {
            *sockopt = ms->ms_sockopt;
        }
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_recv (int sockfd, void * buf, unsigned int count,
                struct sockaddr * addr, unsigned int * addrlenptr,
                void * control, uint64_t * controllenptr)
{
    int retval = 0;
    void * obuf = NULL;
    unsigned int copied;
    unsigned int addrlen = addrlenptr ? *addrlenptr : 0;
    uint64_t controllen  = controllenptr ? *controllenptr : 0;
    ms_ocall_recv_t * ms;

    if ((count + addrlen + controllen) > MAX_UNTRUSTED_STACK_BUF) {
        retval = ocall_mmap_untrusted(-1, 0, ALLOC_ALIGNUP(count), PROT_READ | PROT_WRITE, &obuf);
        if (IS_ERR(retval))
            return retval;
    }

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        retval = -EPERM;
        goto out;
    }

    ms->ms_sockfd = sockfd;
    ms->ms_count = count;
    ms->ms_addrlen = addrlen;
    ms->ms_addr = addr ? sgx_alloc_on_ustack(addrlen) : NULL;
    ms->ms_controllen = controllen;
    ms->ms_control = control ? sgx_alloc_on_ustack(controllen) : NULL;
    if (obuf)
        ms->ms_buf = obuf;
    else
        ms->ms_buf = sgx_alloc_on_ustack(count);

    if (!ms->ms_buf || (addr && !ms->ms_addr)) {
        retval = -EPERM;
        goto out;
    }

    retval = sgx_exitless_ocall(OCALL_RECV, ms);

    if (retval >= 0) {
        if (addr && addrlen) {
            copied = sgx_copy_to_enclave(addr, addrlen, ms->ms_addr, ms->ms_addrlen);
            if (!copied) {
                retval = -EPERM;
                goto out;
            }
            *addrlenptr = copied;
        }

        if (control && controllen) {
            copied = sgx_copy_to_enclave(control, controllen, ms->ms_control, ms->ms_controllen);
            if (!copied) {
                retval = -EPERM;
                goto out;
            }
            *controllenptr = copied;
        }

        if (retval > 0 && !sgx_copy_to_enclave(buf, count, ms->ms_buf, retval)) {
            retval = -EPERM;
            goto out;
        }
    }

out:
    sgx_reset_ustack();
    if (obuf)
        ocall_munmap_untrusted(obuf, ALLOC_ALIGNUP(count));
    return retval;
}

int ocall_send (int sockfd, const void * buf, unsigned int count,
                const struct sockaddr * addr, unsigned int addrlen,
                void * control, uint64_t controllen)
{
    int retval = 0;
    void * obuf = NULL;
    ms_ocall_send_t * ms;

    if (sgx_is_completely_outside_enclave(buf, count)) {
        /* buf is in untrusted memory (e.g., allowed file mmaped in untrusted memory) */
        obuf = (void*)buf;
    } else if (sgx_is_completely_within_enclave(buf, count)) {
        /* typical case of buf inside of enclave memory */
        if ((count + addrlen + controllen) > MAX_UNTRUSTED_STACK_BUF) {
            /* buf is too big and may overflow untrusted stack, so use untrusted heap */
            retval = ocall_mmap_untrusted(-1, 0, ALLOC_ALIGNUP(count), PROT_READ | PROT_WRITE, &obuf);
            if (IS_ERR(retval))
                return retval;
            memcpy(obuf, buf, count);
        }
    } else {
        /* buf is partially in/out of enclave memory */
        return -EPERM;
    }

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        retval = -EPERM;
        goto out;
    }

    ms->ms_sockfd = sockfd;
    ms->ms_count = count;
    ms->ms_addrlen = addrlen;
    ms->ms_addr = addr ? sgx_copy_to_ustack(addr, addrlen) : NULL;
    ms->ms_controllen = controllen;
    ms->ms_control = control ? sgx_copy_to_ustack(control, controllen) : NULL;
    if (obuf)
        ms->ms_buf = obuf;
    else
        ms->ms_buf = sgx_copy_to_ustack(buf, count);

    if (!ms->ms_buf || (addr && !ms->ms_addr)) {
        retval = -EPERM;
        goto out;
    }

    retval = sgx_exitless_ocall(OCALL_SEND, ms);

out:
    sgx_reset_ustack();
    if (obuf && obuf != buf)
        ocall_munmap_untrusted(obuf, ALLOC_ALIGNUP(count));
    return retval;
}

int ocall_setsockopt (int sockfd, int level, int optname,
                      const void * optval, unsigned int optlen)
{
    int retval = 0;
    ms_ocall_setsockopt_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_sockfd = sockfd;
    ms->ms_level = level;
    ms->ms_optname = optname;
    ms->ms_optlen = 0;
    ms->ms_optval = NULL;

    if (optval && optlen > 0) {
        ms->ms_optlen = optlen;
        ms->ms_optval = sgx_copy_to_ustack(optval, optlen);

        if (!ms->ms_optval) {
            sgx_reset_ustack();
            return -EPERM;
        }
    }

    retval = sgx_exitless_ocall(OCALL_SETSOCKOPT, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_shutdown (int sockfd, int how)
{
    int retval = 0;
    ms_ocall_shutdown_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_sockfd = sockfd;
    ms->ms_how = how;

    retval = sgx_exitless_ocall(OCALL_SHUTDOWN, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_gettime (unsigned long * microsec)
{
    int retval = 0;
    ms_ocall_gettime_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    do {
        retval = sgx_exitless_ocall(OCALL_GETTIME, ms);
    } while(retval == -EINTR);
    if (!retval)
        *microsec = ms->ms_microsec;

    sgx_reset_ustack();
    return retval;
}

int ocall_sleep (unsigned long * microsec)
{
    int retval = 0;
    ms_ocall_sleep_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_microsec = microsec ? *microsec : 0;

    /* NOTE: no reason to use exitless for sleep() */
    retval = sgx_ocall(OCALL_SLEEP, ms);
    if (microsec) {
        if (!retval)
            *microsec = 0;
        else if (retval == -EINTR)
            *microsec = ms->ms_microsec;
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_poll(struct pollfd* fds, int nfds, int64_t timeout_us) {
    int retval = 0;
    unsigned int nfds_bytes = nfds * sizeof(struct pollfd);
    ms_ocall_poll_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_nfds = nfds;
    ms->ms_timeout_us = timeout_us;
    ms->ms_fds = sgx_copy_to_ustack(fds, nfds_bytes);

    if (!ms->ms_fds) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_POLL, ms);

    if (retval >= 0) {
        if (!sgx_copy_to_enclave(fds, nfds_bytes, ms->ms_fds, nfds_bytes)) {
            sgx_reset_ustack();
            return -EPERM;
        }
    }

    sgx_reset_ustack();
    return retval;
}

int ocall_rename (const char * oldpath, const char * newpath)
{
    int retval = 0;
    int oldlen = oldpath ? strlen(oldpath) + 1 : 0;
    int newlen = newpath ? strlen(newpath) + 1 : 0;
    ms_ocall_rename_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_oldpath = sgx_copy_to_ustack(oldpath, oldlen);
    ms->ms_newpath = sgx_copy_to_ustack(newpath, newlen);

    if (!ms->ms_oldpath || !ms->ms_newpath) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_RENAME, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_delete (const char * pathname)
{
    int retval = 0;
    int len = pathname ? strlen(pathname) + 1 : 0;
    ms_ocall_delete_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_pathname = sgx_copy_to_ustack(pathname, len);
    if (!ms->ms_pathname) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_DELETE, ms);

    sgx_reset_ustack();
    return retval;
}

int ocall_load_debug(const char * command)
{
    int retval = 0;
    int len = strlen(command) + 1;

    const char * ms = sgx_copy_to_ustack(command, len);
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    retval = sgx_exitless_ocall(OCALL_LOAD_DEBUG, (void *) ms);

    sgx_reset_ustack();
    return retval;
}

/*
 * ocall_get_attestation() triggers remote attestation in untrusted PAL (see sgx_platform.c:
 * retrieve_verified_quote()). If the OCall returns successfully, the function returns
 * attestation data required for platform verification (i.e., sgx_attestation_t). Except the
 * QE report, most data fields of the attestation need to be copied into the enclave.
 *
 * @spid:        The client SPID registered with the IAS.
 * @subkey:      SPID subscription key.
 * @linkable:    Whether the SPID is linkable.
 * @report:      Local attestation report for the quoting enclave.
 * @nonce:       Randomly-generated nonce for freshness.
 * @attestation: Returns the attestation data (QE report, quote, IAS report, signature,
 *               and certificate chain).
 */
int ocall_get_attestation (const sgx_spid_t* spid, const char* subkey, bool linkable,
                           const sgx_report_t* report, const sgx_quote_nonce_t* nonce,
                           sgx_attestation_t* attestation) {

    ms_ocall_get_attestation_t * ms;
    int retval = -EPERM;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms)
        goto reset;

    memcpy(&ms->ms_spid,   spid,   sizeof(sgx_spid_t));
    ms->ms_subkey = sgx_copy_to_ustack(subkey, strlen(subkey) + 1);
    memcpy(&ms->ms_report, report, sizeof(sgx_report_t));
    memcpy(&ms->ms_nonce,  nonce,  sizeof(sgx_quote_nonce_t));
    ms->ms_linkable = linkable;

    retval = sgx_exitless_ocall(OCALL_GET_ATTESTATION, ms);

    if (retval >= 0) {
        // First, try to copy the whole ms->ms_attestation inside
        if (!sgx_copy_to_enclave(attestation, sizeof(sgx_attestation_t), &ms->ms_attestation,
                                 sizeof(sgx_attestation_t))) {
            retval = -EACCES;
            goto reset;
        }

        // For calling ocall_munmap_untrusted, need to reset the untrusted stack
        sgx_reset_ustack();

        // Copy each field inside and free the untrusted buffers
        if (attestation->quote) {
            size_t len = attestation->quote_len;
            sgx_quote_t* quote = malloc(len);
            if (!sgx_copy_to_enclave(quote, len, attestation->quote, len))
                retval = -EACCES;
            ocall_munmap_untrusted(attestation->quote, ALLOC_ALIGNUP(len));
            attestation->quote = quote;
        }

        if (attestation->ias_report) {
            size_t len = attestation->ias_report_len;
            char* ias_report = malloc(len + 1);
            if (!sgx_copy_to_enclave(ias_report, len, attestation->ias_report, len))
                retval = -EACCES;
            ocall_munmap_untrusted(attestation->ias_report, ALLOC_ALIGNUP(len));
            ias_report[len] = 0; // Ensure null-ending
            attestation->ias_report = ias_report;
        }

        if (attestation->ias_sig) {
            size_t len = attestation->ias_sig_len;
            uint8_t* ias_sig = malloc(len);
            if (!sgx_copy_to_enclave(ias_sig, len, attestation->ias_sig, len))
                retval = -EACCES;
            ocall_munmap_untrusted(attestation->ias_sig, ALLOC_ALIGNUP(len));
            attestation->ias_sig = ias_sig;
        }

        if (attestation->ias_certs) {
            size_t len = attestation->ias_certs_len;
            char* ias_certs = malloc(len + 1);
            if (!sgx_copy_to_enclave(ias_certs, len, attestation->ias_certs, len))
                retval = -EACCES;
            ocall_munmap_untrusted(attestation->ias_certs, ALLOC_ALIGNUP(len));
            ias_certs[len] = 0; // Ensure null-ending
            attestation->ias_certs = ias_certs;
        }

        // At this point, no field should point to outside the enclave
        if (retval < 0) {
            if (attestation->quote)      free(attestation->quote);
            if (attestation->ias_report) free(attestation->ias_report);
            if (attestation->ias_sig)    free(attestation->ias_sig);
            if (attestation->ias_certs)  free(attestation->ias_certs);
        }

        goto out;
    }

reset:
    sgx_reset_ustack();
out:
    return retval;
}

int ocall_eventfd (unsigned int initval, int flags)
{
    int retval = 0;
    ms_ocall_eventfd_t * ms;

    ms = sgx_alloc_on_ustack_aligned(sizeof(*ms), alignof(*ms));
    if (!ms) {
        sgx_reset_ustack();
        return -EPERM;
    }

    ms->ms_initval = initval;
    ms->ms_flags   = flags;

    retval = sgx_exitless_ocall(OCALL_EVENTFD, ms);

    sgx_reset_ustack();
    return retval;
}
