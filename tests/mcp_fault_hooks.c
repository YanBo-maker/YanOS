/* Test-only link/compile seams. Production yan_mcp contains none of these
 * environment switches. Real cleanup always runs before a reported failure.
 *
 * mcp_fault_probe links this file; yan_mcp never does. The observer and the
 * block hold below therefore exist only in the fault-probe executable:
 *   YAN_MCP_TEST_BLOCK_OBSERVER=<path>
 *       parent appends "fork <worker-pid>"; the worker appends one
 *       "child <pid> opens=<n> source_opens=<n> preads=<n> block_inits=<n>"
 *       line when it exits through yan_mcp_test_exit().
 *   YAN_MCP_TEST_BLOCK_HOLD=<path>
 *       the block-query worker waits until <path> exists (bounded 15s) while
 *       staying alive, so BUSY/cancel/reap ordering is deterministic.
 * The open/pread/fork interposers are linked into this probe only; they count
 * direct source calls made by yan_mcp/mcp_worker/semantic_posix themselves.
 * libc-internal fopen/read calls are not counted. */
#undef _exit
#undef clock_gettime
#undef poll
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "mcp_worker.h"
#include "semantic_posix.h"
#include "semantic_llama.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
YanSemanticResult __real_yan_semantic_posix_cleanup(YanSemanticPosix *);
YanSemanticResult __real_yan_semantic_llama_stop(void *);
void *__real_calloc(size_t,size_t);
ssize_t __real_write(int,const void *,size_t);
ssize_t __real_read(int,void *,size_t);
int __real_close(int);
FILE *__real_fopen(const char *,const char *);
int __real_poll(struct pollfd *,nfds_t,int);
static pid_t parent_pid;
static int result_writer=-1,result_reader=-1;
static int64_t clock_offset;
static unsigned long observer_opens,observer_source_opens,observer_preads,observer_blocks;
static bool observer_inside;
static int frame_fd=-1;
static int frame_body;
enum {
    FRAME_NONE=0, FRAME_BAD_DOMAIN, FRAME_BAD_RESULT,
    FRAME_TRUNCATED_HEADER, FRAME_TRUNCATED_BODY, FRAME_EXTRA_BYTES
};
enum { BODY_NORMAL=0, BODY_SUPPRESS, BODY_TRUNCATED, BODY_EXTRA };
static bool observer_child(void)
{
    return parent_pid && getpid()!=parent_pid;
}
static bool observer_enabled(void)
{
    return getenv("YAN_MCP_TEST_BLOCK_OBSERVER")!=NULL;
}
/* Records one observer line. The record itself is not part of the observed
 * source I/O: fopen goes through libc, and observer_inside suppresses counting
 * should a platform route it through the interposed symbols. */
static void observer_append(const char *format,...)
{
    const char *path=getenv("YAN_MCP_TEST_BLOCK_OBSERVER");
    if (!path || observer_inside) return;
    observer_inside=true;
    FILE *file=__real_fopen(path,"a");
    if (file) {
        va_list args;
        va_start(args,format);
        vfprintf(file,format,args);
        va_end(args);
        fclose(file);
    }
    observer_inside=false;
}
YanSemanticResult __wrap_yan_semantic_posix_cleanup(YanSemanticPosix *p)
{
    YanSemanticResult r=__real_yan_semantic_posix_cleanup(p);
    return getenv("YAN_MCP_TEST_CLEANUP_FAILURE") ? YAN_SEMANTIC_IO : r;
}
YanSemanticResult __wrap_yan_semantic_llama_stop(void *p)
{
    YanSemanticResult r=__real_yan_semantic_llama_stop(p);
    return getenv("YAN_MCP_TEST_STOP_FAILURE") ? YAN_SEMANTIC_IO : r;
}
void *__wrap_calloc(size_t n,size_t size)
{
    if (!parent_pid) parent_pid=getpid();
    if (getpid()!=parent_pid && getenv("YAN_MCP_TEST_WORKER_ALLOC_FAILURE")) return NULL;
    void *p=__real_calloc(n,size);
    if (observer_child() && p && n==1 && size==sizeof(YanBlockQuery)) {
        ++observer_blocks;
        const char *delay=getenv("YAN_MCP_TEST_BLOCK_DELAY");
        if (delay) {
            struct timespec wait={1,0};
            nanosleep(&wait,NULL);
        }
        const char *hold=getenv("YAN_MCP_TEST_BLOCK_HOLD");
        if (hold) {
            observer_append("hold %ld\n",(long)getpid());
            struct timespec pause={0,5*1000*1000};
            for (unsigned i=0;i<3000;++i) {
                if (access(hold,F_OK)==0) break;
                (void)nanosleep(&pause,NULL);
            }
        }
    }
    return p;
}
_Noreturn void yan_mcp_test_exit(int code)
{
    if (observer_child() && observer_enabled())
        observer_append("child %ld opens=%lu source_opens=%lu preads=%lu block_inits=%lu\n",
            (long)getpid(),observer_opens,observer_source_opens,observer_preads,
            observer_blocks);
    _exit(code==0 && getenv("YAN_MCP_TEST_WORKER_EXIT_FAILURE") ? 9 : code);
}
static int requested_frame(void)
{
    const char *mode=getenv("YAN_MCP_TEST_WORKER_FRAME");
    if (mode) {
        if (!strcmp(mode,"bad-domain")) return FRAME_BAD_DOMAIN;
        if (!strcmp(mode,"bad-result")) return FRAME_BAD_RESULT;
        if (!strcmp(mode,"truncated-header")) return FRAME_TRUNCATED_HEADER;
        if (!strcmp(mode,"truncated-body")) return FRAME_TRUNCATED_BODY;
        if (!strcmp(mode,"extra-bytes")) return FRAME_EXTRA_BYTES;
    }
    if (getenv("YAN_MCP_TEST_WORKER_BAD_DOMAIN")) return FRAME_BAD_DOMAIN;
    return FRAME_NONE;
}
static bool raw_write(int fd,const void *p,size_t n)
{
    const char *bytes=p;
    while (n) {
        ssize_t got=__real_write(fd,bytes,n);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) return false;
        bytes+=got;
        n-=(size_t)got;
    }
    return true;
}
ssize_t __wrap_write(int fd,const void *p,size_t n)
{
    if (getpid()!=parent_pid) {
        if (n==sizeof(YanMcpWorkerHeader) &&
            getenv("YAN_MCP_TEST_WRITER_CLOSE_FAILURE")) result_writer=fd;
        if (n==sizeof(YanMcpWorkerHeader)) {
            int mode=requested_frame();
            if (mode!=FRAME_NONE) {
                YanMcpWorkerHeader header;
                memcpy(&header,p,sizeof header);
                frame_fd=fd;
                frame_body=BODY_NORMAL;
                switch (mode) {
                case FRAME_BAD_DOMAIN:
                    header.domain=2;
                    return __real_write(fd,&header,sizeof header);
                case FRAME_BAD_RESULT:
                    header.result=18; /* > YAN_BLOCK_QUERY_UNSUPPORTED */
                    header.length=0;
                    frame_body=BODY_SUPPRESS;
                    break;
                case FRAME_TRUNCATED_HEADER:
                    (void)raw_write(fd,&header,5);
                    frame_body=BODY_SUPPRESS;
                    return (ssize_t)n;
                case FRAME_TRUNCATED_BODY:
                    frame_body=BODY_TRUNCATED;
                    break;
                case FRAME_EXTRA_BYTES:
                    frame_body=BODY_EXTRA;
                    break;
                default:
                    break;
                }
                if (!raw_write(fd,&header,sizeof header)) return -1;
                return (ssize_t)n;
            }
        } else if (frame_fd==fd && frame_body!=BODY_NORMAL) {
            if (frame_body==BODY_SUPPRESS) return (ssize_t)n;
            if (frame_body==BODY_TRUNCATED) {
                size_t cut=n<20u?n:20u;
                (void)raw_write(fd,p,cut);
                return (ssize_t)n;
            }
            static const char extra[4]={'J','U','N','K'};
            if (!raw_write(fd,p,n) || !raw_write(fd,extra,sizeof extra)) return -1;
            return (ssize_t)n;
        }
    }
    return __real_write(fd,p,n);
}
ssize_t __wrap_read(int fd,void *p,size_t n)
{
    ssize_t got=__real_read(fd,p,n);
    if (getpid()==parent_pid && fd>=3 && got==0 && getenv("YAN_MCP_TEST_READER_CLOSE_FAILURE")) result_reader=fd;
    return got;
}
int __wrap_close(int fd)
{
    int rc=__real_close(fd);
    if ((fd==result_writer && getenv("YAN_MCP_TEST_WRITER_CLOSE_FAILURE")) ||
        (fd==result_reader && getenv("YAN_MCP_TEST_READER_CLOSE_FAILURE"))) { errno=EIO; return -1; }
    return rc;
}
FILE *__wrap_fopen(const char *path,const char *mode)
{
    static bool failed;
    if (!failed && !strcmp(path,"/proc/thread-self/children") && getenv("YAN_MCP_TEST_PROC_FAILURE")) {
        failed=true; errno=EIO; return NULL;
    }
    return __real_fopen(path,mode);
}
int __wrap_poll(struct pollfd *fds,nfds_t count,int timeout)
{
    int rc=__real_poll(fds,count,timeout);
    if (rc>0 && count==3 && fds[1].fd==1 && (fds[1].revents&POLLOUT) && getenv("YAN_MCP_TEST_EXPIRED_OUTPUT")) clock_offset=6000;
    return rc;
}
int yan_mcp_test_clock_gettime(clockid_t which,struct timespec *time)
{
    int rc=clock_gettime(which,time);
    if (!rc && which==CLOCK_MONOTONIC) time->tv_sec+=clock_offset/1000;
    return rc;
}
/* Direct symbol interposition (not --wrap) for calls made by the linked
 * yan_mcp/mcp_worker/semantic_posix objects. The syscall forms below avoid
 * re-entering these functions. libc-internal file work is untouched. */
int open(const char *path,int flags,...)
{
    mode_t mode=0;
#ifdef O_TMPFILE
    if (flags&(O_CREAT|O_TMPFILE)) {
#else
    if (flags&O_CREAT) {
#endif
        va_list args;
        va_start(args,flags);
        mode=(mode_t)va_arg(args,int);
        va_end(args);
    }
    if (observer_child()) {
        if (!observer_inside) {
            ++observer_opens;
            if (strcmp(path,"/dev/null")) ++observer_source_opens;
        }
    } else if (observer_enabled() && !observer_inside && strcmp(path,"/dev/null")) {
        /* Source I/O must happen in the forked block worker, never here. */
        observer_append("parent-open\n");
    }
    return (int)syscall(SYS_openat,AT_FDCWD,path,(long)flags,(long)mode);
}
ssize_t pread(int fd,void *p,size_t n,off_t offset)
{
    if (observer_child()) {
        if (!observer_inside) ++observer_preads;
    } else if (observer_enabled() && !observer_inside) {
        observer_append("parent-pread\n");
    }
#ifdef SYS_pread64
    return (ssize_t)syscall(SYS_pread64,fd,p,n,(long)offset);
#else
    return (ssize_t)syscall(SYS_pread,fd,p,n,(long)offset);
#endif
}
pid_t fork(void)
{
#ifdef SYS_fork
    pid_t pid=(pid_t)syscall(SYS_fork);
#else
    pid_t pid=(pid_t)syscall(SYS_clone,(unsigned long)SIGCHLD,0,0,0,0);
#endif
    if (pid>0 && observer_enabled()) observer_append("fork %ld\n",(long)pid);
    return pid;
}
