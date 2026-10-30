#define _GNU_SOURCE
#include "mcp_note.h"
#include "mcp_worker.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void interrupt(int sig) { interrupted=sig; }
static volatile sig_atomic_t worker_cancelled;
static void worker_cancel(int sig) { (void)sig; worker_cancelled=1; }
static int64_t now_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC,&time)) return -1;
    return (int64_t)time.tv_sec*1000+time.tv_nsec/1000000;
}
static bool seconds(const char *s,unsigned *out)
{
    unsigned n=0; if (!*s) return false;
    for (;*s;++s) { if (*s<'0' || *s>'9' || n>360) return false; n=n*10+(unsigned)(*s-'0'); if (n>3600) return false; }
    if (!n) return false;
    *out=n; return true;
}
/* --allow-meta exists only to protect the default byte-identical tool list:
 * 0035-R1-AC1 and 0035-R12-AC1 require tools/list to match the 0033 six-tool
 * array exactly when no extension flag is given, so the optional 0027
 * metadata tool must stay off by default. It is not a product read toggle and
 * it deliberately takes no value and no bit in the required-argument bitmap,
 * so --disk-image alone still selects the single image served (0035-R11). */
static bool options(int argc,char **argv,YanMcpOptions *o,bool *allow_write,
                    bool *allow_meta)
{
    memset(o,0,sizeof *o); o->startup=o->request=120; unsigned seen=0;
    for (int i=1;i<argc;) {
        if (!strcmp(argv[i],"--allow-write")) {
            if (*allow_write) return false;
            *allow_write=true; ++i; continue;
        }
        if (!strcmp(argv[i],"--allow-meta")) {
            if (*allow_meta) return false;
            *allow_meta=true; ++i; continue;
        }
        if (i+1>=argc) return false;
        unsigned bit; const char **p=NULL;
        if (!strcmp(argv[i],"--disk-image")) { bit=1; p=&o->image; }
        else if (!strcmp(argv[i],"--index")) { bit=2; p=&o->index; }
        else if (!strcmp(argv[i],"--model")) { bit=4; p=&o->model; }
        else if (!strcmp(argv[i],"--runner")) { bit=8; p=&o->runner; }
        else if (!strcmp(argv[i],"--startup-timeout")) bit=16;
        else if (!strcmp(argv[i],"--request-timeout")) bit=32;
        else return false;
        if ((seen&bit) || !*argv[i+1]) return false;
        seen|=bit;
        if (p) *p=argv[i+1]; else if (!seconds(argv[i+1],bit==16?&o->startup:&o->request)) return false;
        i+=2;
    }
    return (seen&15)==15;
}
typedef struct Frame {
    struct Frame *next; char *data; size_t length,sent;
    int64_t deadline; bool task; YanMcpId id;
} Frame;
typedef struct {
    bool active,cancelled,stopping,killed,grouped,eof,failed,leader_exited,orphan,transport_failed;
    pid_t pid; int fd,status; int64_t stop_time;
    YanMcpRequest request; char *data; size_t received;
} Task;
typedef struct {
    YanMcpSession session; YanMcpOptions options; Task task;
    Frame *head,*tail; size_t queued;
    char input[YAN_MCP_INPUT_MAX+1]; size_t input_length; bool discard,closing;
    int exit_code;
} Server;
/* Linux consumes an owned fd even when close reports EINTR/late I/O failure.
 * Never retry close against a descriptor that could have been reused. */
static bool close_fd(int *fd)
{ int owned=*fd; *fd=-1; return owned<0 || close(owned)==0; }
static bool enqueue(Server *s,const YanMcpRequest *request,const char *payload,size_t n,const char *token,bool task)
{
    size_t length=0; char *data=NULL;
    YanMcpReplyStatus status=yan_mcp_reply_ex(request,payload,n,token,&data,&length,NULL);
    if (status!=YAN_MCP_REPLY_OK) {
        free(data); data=NULL; length=0;
        const char *fallback=status==YAN_MCP_REPLY_LIMIT ? "LIMIT" : "IO";
        if (request->kind==YAN_MCP_BLOCK_LIST || request->kind==YAN_MCP_BLOCK_READ ||
            request->kind==YAN_MCP_BLOCK_REFS || request->kind==YAN_MCP_BLOCK_BACKLINKS)
            data=yan_mcp_reply(request,NULL,0,fallback,&length,NULL);
    }
    if (!data || length>YAN_MCP_QUEUE_MAX-s->queued) { free(data); return false; }
    Frame *frame=calloc(1,sizeof *frame);
    int64_t now=now_ms();
    if (!frame || now<0) { free(frame); free(data); return false; }
    frame->data=data; frame->length=length; frame->task=task; frame->deadline=now+5000; frame->id=request->id;
    if (s->tail) s->tail->next=frame; else s->head=frame;
    s->tail=frame; s->queued+=length; return true;
}
static void pop_frame(Server *s)
{
    Frame *frame=s->head; s->head=frame->next;
    if (!s->head) s->tail=NULL;
    s->queued-=frame->length;
    free(frame->data); free(frame);
}
static bool discard_task_frame(Server *s)
{
    Frame *previous=NULL;
    for (Frame *frame=s->head;frame;frame=frame->next) {
        if (!frame->task) { previous=frame; continue; }
        if (frame->sent) return false;
        if (previous) previous->next=frame->next; else s->head=frame->next;
        if (s->tail==frame) s->tail=previous;
        s->queued-=frame->length; free(frame->data); free(frame); break;
    }
    return true;
}
static void stop_task(Task *t,bool cancelled)
{
    if (!t->active) return;
    if (cancelled) t->cancelled=true;
    if (t->pid>0 && !t->stopping) {
        t->stopping=true; t->stop_time=now_ms();
        if (kill(t->grouped ? -t->pid : t->pid,SIGTERM) && errno!=ESRCH) t->failed=true;
    }
}
static void shutdown_server(Server *s,int code)
{
    s->closing=true;
    if (s->exit_code!=5 && code) s->exit_code=code;
    if (!discard_task_frame(s)) s->exit_code=5;
    while (s->head) pop_frame(s);
    stop_task(&s->task,true);
    if (s->task.active && s->task.pid<=0) s->task.active=false;
}
static bool write_all(int fd,const void *bytes,size_t length)
{
    const char *p=bytes;
    while (length) {
        ssize_t n=write(fd,p,length);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return false;
        p+=n; length-=(size_t)n;
    }
    return true;
}
static void child(const YanMcpOptions *o,const YanMcpRequest *request,int result_fd,int gate_fd,const sigset_t *mask)
{
    struct sigaction action={0}; action.sa_handler=worker_cancel; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL) || setpgid(0,0)) _exit(5);
    int null_fd=open("/dev/null",O_RDWR|O_CLOEXEC);
    if (null_fd<0 || dup2(null_fd,STDIN_FILENO)<0 || dup2(null_fd,STDOUT_FILENO)<0) _exit(5);
    if (null_fd>STDERR_FILENO && !close_fd(&null_fd)) _exit(5);
    if (sigprocmask(SIG_SETMASK,mask,NULL)) _exit(5);
    char go=0; ssize_t n;
    do { n=read(gate_fd,&go,1); } while (n<0 && errno==EINTR && !worker_cancelled);
    if (!close_fd(&gate_fd)) _exit(5);
    if (n!=1 || go!='G' || worker_cancelled) _exit(5);
    char *payload=NULL; size_t length=0;
    YanMcpWorkerResult result=yan_mcp_execute(o,request,&worker_cancelled,&payload,&length);
    if (length>YAN_MCP_OUTPUT_MAX) {
        free(payload); payload=NULL; length=0;
        result.result=result.domain==YAN_MCP_DOMAIN_BLOCK ?
            (uint32_t)YAN_BLOCK_QUERY_LIMIT : (uint32_t)YAN_SEMANTIC_IO;
    }
    YanMcpWorkerHeader header={(uint32_t)result.domain,result.result,(uint32_t)length};
    bool sent=write_all(result_fd,&header,sizeof header) && (!length || write_all(result_fd,payload,length));
    free(payload); bool closed=close_fd(&result_fd); _exit(sent && closed?0:5);
}
static bool start_task(Server *s,const YanMcpRequest *request)
{
    Task *t=&s->task;
    memset(t,0,sizeof *t); t->fd=-1; t->request=*request;
    t->data=malloc(sizeof(YanMcpWorkerHeader)+YAN_MCP_OUTPUT_MAX);
    if (!t->data) return false;
    int data[2]={-1,-1},gate[2]={-1,-1}; sigset_t block,old;
    sigemptyset(&block); sigaddset(&block,SIGINT); sigaddset(&block,SIGTERM);
    if (pipe2(data,O_CLOEXEC) || pipe2(gate,O_CLOEXEC)) goto fail;
    if (sigprocmask(SIG_BLOCK,&block,&old)) goto fail;
    pid_t pid=fork();
    if (pid==0) {
        if (!close_fd(&data[0]) || !close_fd(&gate[1])) _exit(5);
        child(&s->options,request,data[1],gate[0],&old);
    }
    if (pid<0) { (void)sigprocmask(SIG_SETMASK,&old,NULL); goto fail; }
    bool closed_data=close_fd(&data[1]), closed_gate=close_fd(&gate[0]);
    t->fd=data[0]; t->pid=pid; t->active=true;
    if (!closed_data || !closed_gate) t->transport_failed=true;
    /* Parent establishes ownership before releasing the gate. The leader is
     * kept unreaped until no further group signals can be necessary. */
    int grouped;
    do { grouped=setpgid(pid,pid); } while (grouped<0 && errno==EINTR);
    t->grouped=grouped==0 && getpgid(pid)==pid;
    if (!closed_data || !closed_gate || !t->grouped || fcntl(t->fd,F_SETFL,O_NONBLOCK)<0 || !write_all(gate[1],"G",1)) {
        t->failed=true; stop_task(t,false);
    }
    if (!close_fd(&gate[1])) { t->transport_failed=true; t->failed=true; stop_task(t,false); }
    if (sigprocmask(SIG_SETMASK,&old,NULL)) { t->failed=true; stop_task(t,false); }
    return true;
fail:
    for (unsigned i=0;i<2;++i) { if (data[i]>=0) close(data[i]); if (gate[i]>=0) close(gate[i]); }
    free(t->data); t->data=NULL; return false;
}
static void read_worker(Task *t)
{
    if (t->fd<0 || t->eof) return;
    const size_t cap=sizeof(YanMcpWorkerHeader)+YAN_MCP_OUTPUT_MAX;
    char scratch[65536];
    ssize_t n=read(t->fd,scratch,sizeof scratch);
    if (n<0) { if (errno!=EINTR && errno!=EAGAIN) { t->failed=true; t->transport_failed=true; stop_task(t,false); } return; }
    if (!n) { t->eof=true; if (!close_fd(&t->fd)) { t->failed=true; t->transport_failed=true; } return; }
    if ((size_t)n>cap-t->received) { t->failed=true; stop_task(t,false); return; }
    memcpy(t->data+t->received,scratch,(size_t)n); t->received+=(size_t)n;
}
/* Once the leader has exited Linux has reparented its orphan descendants to
 * this subreaper. Reap only confirmed members, keeping the leader as a PGID
 * reservation while group signals remain possible. /proc is Linux-specific. */
static bool reap_descendants(Task *t)
{
    FILE *children=fopen("/proc/thread-self/children","r");
    if (!children) { t->failed=true; t->transport_failed=true; return false; }
    long value; bool done=true,seen_owned=false;
    while (fscanf(children,"%ld",&value)==1) {
        if (value<=0 || value==t->pid) continue;
        pid_t pid=(pid_t)value;
        if ((long)pid!=value || getpgid(pid)!=t->pid) continue;
        seen_owned=true;
        int status=0; pid_t got=waitpid(pid,&status,WNOHANG);
        if (got==pid) t->orphan=true;
        else if (got==0 || (got<0 && errno==EINTR)) done=false;
        else if (got<0 && errno!=ECHILD) { t->failed=true; done=false; }
    }
    if (ferror(children)) { t->failed=true; t->transport_failed=true; done=false; }
    if (fclose(children)) { t->failed=true; t->transport_failed=true; done=false; }
    /* Reaping an adopted parent can expose another generation after this
     * proc snapshot. Require a subsequent empty scan before releasing leader. */
    return done && !seen_owned;
}
static bool finish_task(Server *s)
{
    Task *t=&s->task;
    if (!t->active || t->pid<=0) return true;
    if (!t->leader_exited) {
        siginfo_t info={0};
        if (waitid(P_PID,(id_t)t->pid,&info,WEXITED|WNOHANG|WNOWAIT)<0) {
            if (errno!=EINTR) { t->failed=true; shutdown_server(s,5); }
        } else if (info.si_pid==t->pid) t->leader_exited=true;
    }
    int64_t now=now_ms();
    if (now<0) { t->failed=true; shutdown_server(s,5); }
    if (t->leader_exited || (t->stopping && now-t->stop_time>=5000)) {
        if (!t->killed || t->leader_exited) {
            if (kill(t->grouped ? -t->pid : t->pid,SIGKILL) && errno!=ESRCH) t->failed=true;
            t->killed=true;
        }
    }
    if (t->transport_failed) shutdown_server(s,5);
    if (!t->leader_exited) return true;
    bool reaped=reap_descendants(t);
    if (t->transport_failed) shutdown_server(s,5);
    if (!reaped) return true;
    pid_t got=waitpid(t->pid,&t->status,WNOHANG);
    if (got!=t->pid) return true;
    t->pid=0;
    /* With every owned writer dead, drain remaining pipe data through EOF. */
    while (t->fd>=0 && !t->eof) {
        size_t before=t->received; read_worker(t);
        if (t->received==before && !t->eof) { t->failed=true; t->transport_failed=true; (void)close_fd(&t->fd); break; }
    }
    if (t->transport_failed) shutdown_server(s,5);
    if (t->cancelled || s->closing) { free(t->data); t->data=NULL; t->active=false; return true; }
    YanMcpWorkerHeader header={0};
    bool valid=!t->failed && !t->orphan && WIFEXITED(t->status) && WEXITSTATUS(t->status)==0 &&
        t->received>=sizeof header;
    if (valid) memcpy(&header,t->data,sizeof header);
    valid=valid && yan_mcp_worker_frame_valid(&header,t->received);
    const char *token=!valid ? "IO" : yan_mcp_worker_token(&header);
    bool queued=enqueue(s,&t->request,token?NULL:t->data+sizeof header,token?0:header.length,token,true);
    free(t->data); t->data=NULL;
    if (!queued) return false;
    return true;
}
static bool handle(Server *s,const char *line,size_t length)
{
    YanMcpRequest request;
    YanMcpSession pending=s->session;
    yan_mcp_parse(line,length,&pending,s->task.active?&s->task.request.id:NULL,&request,NULL);
    bool duplicate=false;
    for (Frame *frame=s->head;frame;frame=frame->next) {
        if (yan_mcp_id_equal(&frame->id,&request.id)) { duplicate=true; break; }
    }
    if (duplicate) { request.kind=YAN_MCP_PROTOCOL_ERROR; request.error=-32600; }
    else s->session=pending;
    if (request.kind==YAN_MCP_IGNORE) return true;
    if (request.kind==YAN_MCP_CANCEL) {
        if (s->task.active && yan_mcp_id_equal(&request.target,&s->task.request.id)) {
            if (!discard_task_frame(s)) return false;
            stop_task(&s->task,true);
            if (s->task.pid<=0) s->task.active=false;
        }
        return true;
    }
    if (request.kind==YAN_MCP_NOTE_CREATE || request.kind==YAN_MCP_NOTE_REPLACE) {
        if (s->task.active) return enqueue(s,&request,NULL,0,"BUSY",false);
        YanMcpNoteRequest note={.replace=request.kind==YAN_MCP_NOTE_REPLACE,
            .dry_run=request.dry_run,.has_expected=request.has_expected_source,
            .name=request.name,.text=request.text,.length=request.text_length};
        memcpy(note.expected,request.expected_source,sizeof note.expected);
        char *payload=NULL; size_t payload_length=0; bool committed=false;
        YanMcpNoteResult result=yan_mcp_note_execute(s->options.image,&note,&payload,
            &payload_length,&committed);
        const char *token=result==YAN_MCP_NOTE_OK?NULL:yan_mcp_note_result_name(result);
        request.committed=committed;
        bool queued=enqueue(s,&request,token?NULL:payload,token?0:payload_length,token,false);
        free(payload);
        return queued;
    }
    if (request.kind==YAN_MCP_SEARCH || request.kind==YAN_MCP_STATUS ||
        request.kind==YAN_MCP_NOTE_META ||
        request.kind==YAN_MCP_BLOCK_LIST || request.kind==YAN_MCP_BLOCK_READ ||
        request.kind==YAN_MCP_BLOCK_REFS || request.kind==YAN_MCP_BLOCK_BACKLINKS) {
        if (s->task.active) return enqueue(s,&request,NULL,0,"BUSY",false);
        if (!start_task(s,&request)) return enqueue(s,&request,NULL,0,"IO",false);
        return true;
    }
    return enqueue(s,&request,NULL,0,NULL,false);
}
static bool read_input(Server *s)
{
    char data[65536]; ssize_t n=read(STDIN_FILENO,data,sizeof data);
    if (n<0) return errno==EINTR || errno==EAGAIN;
    if (!n) { shutdown_server(s,0); return true; }
    for (ssize_t i=0;i<n;++i) {
        char c=data[i];
        if (c=='\n') {
            size_t length=s->input_length;
            if (length && s->input[length-1]=='\r') --length;
            bool ok=handle(s,s->discard?NULL:s->input,s->discard?0:length);
            s->input_length=0; s->discard=false;
            if (!ok) return false;
        } else if (!s->discard) {
            if (s->input_length<sizeof s->input) s->input[s->input_length++]=c;
            else s->discard=true;
        }
    }
    return true;
}
static bool write_output(Server *s)
{
    Frame *frame=s->head; if (!frame) return true;
    int64_t now=now_ms();
    if (now<0 || now>=frame->deadline) return false;
    size_t left=frame->length-frame->sent; if (left>65536) left=65536;
    ssize_t n=write(STDOUT_FILENO,frame->data+frame->sent,left);
    if (n<0) return errno==EINTR || errno==EAGAIN;
    if (!n) return false;
    frame->sent+=(size_t)n;
    now=now_ms();
    if (now<0 || now>=frame->deadline) return false;
    if (frame->sent==frame->length) {
        if (frame->task) s->task.active=false;
        pop_frame(s);
    }
    return true;
}
int main(int argc,char **argv)
{
    Server *s=calloc(1,sizeof *s);
    if (!s) { fputs("ERROR IO\n",stderr); return 5; }
    bool allow_write=false, allow_meta=false;
    if (!options(argc,argv,&s->options,&allow_write,&allow_meta)) { free(s); fputs("ERROR USAGE\n",stderr); return 2; }
    s->session.allow_write=allow_write;
    s->session.allow_meta=allow_meta;
    struct sigaction action={0}; action.sa_handler=interrupt; sigemptyset(&action.sa_mask);
    bool ready=sigaction(SIGINT,&action,NULL)==0 && sigaction(SIGTERM,&action,NULL)==0;
    /* The harness may ignore child exits or block control signals. Own our
     * dispositions/mask: waitable leaders reserve PGIDs until cleanup ends. */
    action.sa_handler=SIG_DFL;
    ready=ready && sigaction(SIGCHLD,&action,NULL)==0;
    sigset_t controlled;
    sigemptyset(&controlled); sigaddset(&controlled,SIGINT);
    sigaddset(&controlled,SIGTERM); sigaddset(&controlled,SIGCHLD);
    ready=ready && sigprocmask(SIG_UNBLOCK,&controlled,NULL)==0;
    action.sa_handler=SIG_IGN;
    ready=ready && sigaction(SIGPIPE,&action,NULL)==0 && prctl(PR_SET_CHILD_SUBREAPER,1)==0;
    int input_flags=fcntl(STDIN_FILENO,F_GETFL), output_flags=fcntl(STDOUT_FILENO,F_GETFL);
    ready=ready && input_flags>=0 && output_flags>=0 &&
        fcntl(STDIN_FILENO,F_SETFL,input_flags|O_NONBLOCK)==0 && fcntl(STDOUT_FILENO,F_SETFL,output_flags|O_NONBLOCK)==0;
    if (!ready) { free(s); fputs("ERROR IO\n",stderr); return 5; }
    while (!s->closing || (s->task.active && s->task.pid>0)) {
        if (interrupted) shutdown_server(s,interrupted==SIGINT?130:143);
        struct pollfd fds[3]={{s->closing?-1:STDIN_FILENO,POLLIN,0},
            {s->closing || !s->head?-1:STDOUT_FILENO,POLLOUT,0}, {s->task.pid>0?s->task.fd:-1,POLLIN,0}};
        int rc=poll(fds,3,50);
        if (rc<0 && errno!=EINTR) shutdown_server(s,5);
        if (!s->closing && (fds[0].revents&(POLLIN|POLLHUP)) && !read_input(s)) shutdown_server(s,5);
        if (fds[0].revents&(POLLERR|POLLNVAL)) shutdown_server(s,5);
        if (s->task.pid>0 && (fds[2].revents&(POLLIN|POLLHUP|POLLERR))) read_worker(&s->task);
        if (!finish_task(s)) shutdown_server(s,5);
        if (!s->closing && ((fds[1].revents&(POLLERR|POLLHUP|POLLNVAL)) ||
            ((fds[1].revents&POLLOUT) && !write_output(s)))) shutdown_server(s,5);
        int64_t now=now_ms();
        if (now<0 || (s->head && now>=s->head->deadline)) shutdown_server(s,5);
    }
    while (s->head) pop_frame(s);
    free(s->task.data);
    (void)fcntl(STDIN_FILENO,F_SETFL,input_flags); (void)fcntl(STDOUT_FILENO,F_SETFL,output_flags);
    int code=s->exit_code; free(s);
    if (code==5) fputs("ERROR OUTPUT_OR_IO\n",stderr);
    return code;
}
