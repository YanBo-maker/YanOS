/* Test-only link/compile seams. Production yan_mcp contains none of these
 * environment switches. Real cleanup always runs before a reported failure. */
#undef _exit
#undef clock_gettime
#undef poll
#define _POSIX_C_SOURCE 200809L
#include "semantic_posix.h"
#include "semantic_llama.h"
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
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
    return __real_calloc(n,size);
}
_Noreturn void yan_mcp_test_exit(int code)
{ _exit(code==0 && getenv("YAN_MCP_TEST_WORKER_EXIT_FAILURE") ? 9 : code); }
ssize_t __wrap_write(int fd,const void *p,size_t n)
{
    if (getpid()!=parent_pid && n==8 && getenv("YAN_MCP_TEST_WRITER_CLOSE_FAILURE")) result_writer=fd;
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
