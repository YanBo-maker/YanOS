/* 0029 optional Linux provider: one owned llama.cpp child plus a bounded
 * loopback HTTP/JSON client.
 *
 * The contract is docs/specs/0029-host-semantic-search.md, "模型进程与
 * provider边界" and the provider row of VERIFY, plus host/semantic_llama.h.
 * This translation unit owns exactly one child process and one random
 * authentication secret per command. It never downloads a model, never talks
 * to a remote endpoint, never enables a proxy or redirect, and never trusts a
 * pre-existing listener: start reserves a loopback port, verifies it can bind
 * it, releases it, forks/execs the runner with a sanitised LLAMA_ARG_*
 * environment, and polls an authenticated /health until the monotonic startup
 * deadline. Every failure path keeps ownership of the child long enough for
 * stop() to terminate and reap it.
 *
 * config runs `runner --version` in a separate, bounded pipe child and hashes
 * explicit-length TLVs covering the preprocessing versions, 1024 dimensions,
 * Q8_0, last pooling, the exact 0029 prefixes, the fixed inference flags and
 * the actual captured version. It does not load the model and starts no
 * inference server.
 *
 * embed posts one JSON object with yyjson and parses exactly one 1024-value
 * embedding, validating types, finiteness, range and nonzero norm before
 * touching the caller output. The response body is capped at 1 MiB. libcurl
 * handles no proxy/redirect, uses a monotonic deadline and a cancel flag in a
 * 50 ms multi loop, and every easy/multi handle and buffer is released on all
 * paths.
 *
 * Tests-first history: this adapter and its fake-runner probe existed first
 * and were RED (UNSUPPORTED); this source replaced the stub. Real model
 * quality is not claimed here. */

#define _POSIX_C_SOURCE 200809L
#include "semantic_llama.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

_Static_assert(YYJSON_VERSION_HEX == 0x000D00, "0029 pins yyjson 0.13.0");

extern char **environ;

#define YAN_SEMANTIC_MAX_RESPONSE (1024u * 1024u)
#define YAN_SEMANTIC_STOP_GRACE_MS UINT64_C(5000)

static const char yan_query_prefix[] =
    "Instruct: Given a question, retrieve relevant passages from notes.\n"
    "Query: ";
static const char yan_title_prefix[] = "Title: ";
static const char yan_title_separator[] = "\n\n";
static const char yan_quantization[] = "Q8_0";
static const char yan_pooling[] = "last";
static const char yan_inference_flags[] =
    "--embedding --pooling last --ctx-size 4096 --batch-size 4096 "
    "--ubatch-size 4096 --threads 4 --threads-batch 4 --n-gpu-layers 0 "
    "--parallel 1 --alias yanos-embedding --no-webui --no-warmup";

typedef struct {
    const volatile sig_atomic_t *cancel;
    uint64_t deadline_ms;
    bool cancelled;
    bool timed_out;
} YanSemanticRequestControl;

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    bool overflow;
    bool failed;
} YanSemanticResponse;

/* --------------------------------------------------------------- helpers */

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec pause;
    pause.tv_sec = (time_t)(milliseconds / 1000u);
    pause.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&pause, NULL);
}

static bool random_bytes(uint8_t *out, size_t length)
{
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    size_t offset = 0u;
    while (offset < length) {
        const ssize_t got = read(fd, out + offset, length - offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return false;
        }
        if (got == 0) {
            close(fd);
            return false;
        }
        offset += (size_t)got;
    }
    close(fd);
    return true;
}

static bool random_secret(char out[65])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[32];
    if (!random_bytes(bytes, sizeof bytes)) {
        return false;
    }
    for (size_t i = 0u; i < sizeof bytes; ++i) {
        out[2u * i] = hex[(bytes[i] >> 4) & 0x0Fu];
        out[2u * i + 1u] = hex[bytes[i] & 0x0Fu];
    }
    out[64] = 0;
    return true;
}

/* Copies the process environment without any LLAMA_ARG_* entry, so a stray
 * exported override cannot silently change the pinned inference flags. */
static char **sanitized_env(void)
{
    size_t count = 0u;
    while (environ[count] != NULL) {
        ++count;
    }
    char **env = malloc((count + 1u) * sizeof *env);
    if (env == NULL) {
        return NULL;
    }
    size_t output = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if (strncmp(environ[i], "LLAMA_ARG_", 10u) == 0) {
            continue;
        }
        env[output++] = environ[i];
    }
    env[output] = NULL;
    return env;
}

/* Reserves a loopback port: 0 picks an ephemeral port, an explicit value
 * detects a collision. The reservation is released before exec and the child
 * rebinds; a losing race is caught by the authenticated health poll. */
static YanSemanticResult reserve_port(unsigned requested, unsigned *port_out)
{
    if (requested > 65535u) {
        return YAN_SEMANTIC_INVALID;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return YAN_SEMANTIC_PROVIDER;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)requested);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        close(fd);
        return YAN_SEMANTIC_PROVIDER;
    }
    socklen_t length = sizeof address;
    if (getsockname(fd, (struct sockaddr *)&address, &length) != 0) {
        close(fd);
        return YAN_SEMANTIC_PROVIDER;
    }
    *port_out = (unsigned)ntohs(address.sin_port);
    close(fd);
    return YAN_SEMANTIC_OK;
}

static bool curl_ready(void)
{
    static bool ready = false;
    if (!ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            return false;
        }
        ready = true;
    }
    return true;
}

/* ----------------------------------------------------------- HTTP client */

static size_t response_write(char *ptr, size_t size, size_t nmemb,
                             void *userdata)
{
    YanSemanticResponse *response = userdata;
    const size_t bytes = size * nmemb;
    if (response->overflow || response->failed) {
        return 0u;
    }
    if (bytes > YAN_SEMANTIC_MAX_RESPONSE - response->length) {
        response->overflow = true;
        return 0u;
    }
    const size_t needed = response->length + bytes;
    if (needed > response->capacity) {
        size_t capacity = response->capacity != 0u ? response->capacity : 4096u;
        while (capacity < needed) {
            capacity *= 2u;
        }
        uint8_t *grown = realloc(response->data, capacity);
        if (grown == NULL) {
            response->failed = true;
            return 0u;
        }
        response->data = grown;
        response->capacity = capacity;
    }
    memcpy(response->data + response->length, ptr, bytes);
    response->length = needed;
    return bytes;
}

static size_t discard_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    YanSemanticRequestControl *control = clientp;
    if (control->cancel != NULL && *control->cancel) {
        control->cancelled = true;
        return 1;
    }
    if (monotonic_ms() >= control->deadline_ms) {
        control->timed_out = true;
        return 1;
    }
    return 0;
}

/* Runs one loopback request with a monotonic deadline. Returns OK when an
 * HTTP response (possibly a 4xx/5xx) was received or when the connection was
 * refused (http_code 0, caller retries); TIMEOUT/CANCELLED when the deadline
 * or the borrowed cancel flag fired; PROVIDER/NOSPACE otherwise. */
static YanSemanticResult http_perform(YanSemanticLlama *p, const char *url,
                                      struct curl_slist *headers,
                                      const char *post_body, size_t post_length,
                                      YanSemanticResponse *response,
                                      long *http_code, uint64_t deadline)
{
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return YAN_SEMANTIC_PROVIDER;
    }
    YanSemanticRequestControl control;
    control.cancel = p->cancel;
    control.deadline_ms = deadline;
    control.cancelled = false;
    control.timed_out = false;

    if (response != NULL) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &control);
    if (post_body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)post_length);
    }

    CURLM *multi = curl_multi_init();
    if (multi == NULL) {
        curl_easy_cleanup(curl);
        return YAN_SEMANTIC_PROVIDER;
    }
    curl_multi_add_handle(multi, curl);

    int running = 1;
    YanSemanticResult result = YAN_SEMANTIC_OK;
    CURLcode code = CURLE_OK;
    for (;;) {
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            result = YAN_SEMANTIC_PROVIDER;
            break;
        }
        if (control.cancelled || (p->cancel != NULL && *p->cancel)) {
            result = YAN_SEMANTIC_CANCELLED;
            break;
        }
        if (control.timed_out || monotonic_ms() >= deadline) {
            result = YAN_SEMANTIC_TIMEOUT;
            break;
        }
        if (running == 0) {
            break;
        }
        int descriptors = 0;
        if (curl_multi_poll(multi, NULL, 0u, 50, &descriptors) != CURLM_OK) {
            result = YAN_SEMANTIC_PROVIDER;
            break;
        }
        (void)descriptors;
    }
    if (result == YAN_SEMANTIC_OK) {
        int pending = 0;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &pending)) != NULL) {
            if (message->msg == CURLMSG_DONE) {
                code = message->data.result;
            }
        }
        if (control.cancelled) {
            result = YAN_SEMANTIC_CANCELLED;
        } else if (control.timed_out) {
            result = YAN_SEMANTIC_TIMEOUT;
        }
    }

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_easy_cleanup(curl);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    if (response != NULL) {
        if (response->failed) {
            return YAN_SEMANTIC_NOSPACE;
        }
        if (response->overflow) {
            return YAN_SEMANTIC_PROVIDER;
        }
    }
    if (code == CURLE_OK) {
        *http_code = http_status;
        return YAN_SEMANTIC_OK;
    }
    if (code == CURLE_COULDNT_CONNECT || code == CURLE_GOT_NOTHING) {
        *http_code = 0;
        return YAN_SEMANTIC_OK;
    }
    if (code == CURLE_OPERATION_TIMEDOUT ||
        code == CURLE_ABORTED_BY_CALLBACK) {
        return YAN_SEMANTIC_TIMEOUT;
    }
    return YAN_SEMANTIC_PROVIDER;
}

static YanSemanticResult health_request(YanSemanticLlama *p, long *code,
                                        uint64_t deadline)
{
    char url[128];
    char auth[128];
    snprintf(url, sizeof url, "%s/health", p->url);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", p->secret);
    struct curl_slist *headers = curl_slist_append(NULL, auth);
    if (headers == NULL) {
        return YAN_SEMANTIC_NOSPACE;
    }
    YanSemanticResponse response = {0};
    const YanSemanticResult result =
        http_perform(p, url, headers, NULL, 0u, &response, code, deadline);
    free(response.data);
    curl_slist_free_all(headers);
    return result;
}

/* ------------------------------------------------------ child ownership */

static bool child_alive(YanSemanticLlama *p)
{
    if (p->owned_pid <= 0) {
        return false;
    }
    int status = 0;
    pid_t reaped;
    do { reaped = waitpid(p->owned_pid, &status, WNOHANG); }
    while (reaped < 0 && errno == EINTR);
    if (reaped == 0) {
        return true;
    }
    if (reaped == p->owned_pid || (reaped < 0 && errno == ECHILD)) {
        p->owned_pid = -1;
        p->started = false;
    }
    return false;
}

/* TERM then, after a bounded grace, KILL; always reaps the owned pid and
 * clears it so a reused PID is never signalled. Idempotent. */
static YanSemanticResult stop_owned(YanSemanticLlama *p)
{
    if (p->owned_pid <= 0) {
        p->started = false;
        return YAN_SEMANTIC_OK;
    }
    const pid_t pid = p->owned_pid;
    int status = 0;
    pid_t reaped;
    do { reaped = waitpid(pid, &status, WNOHANG); }
    while (reaped < 0 && errno == EINTR);
    if (reaped == pid || (reaped == -1 && errno == ECHILD)) {
        p->owned_pid = -1;
        p->started = false;
        return YAN_SEMANTIC_PROVIDER;
    }
    if (reaped < 0) return YAN_SEMANTIC_PROVIDER;
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) return YAN_SEMANTIC_PROVIDER;
    const uint64_t deadline = monotonic_ms() + YAN_SEMANTIC_STOP_GRACE_MS;
    for (;;) {
        reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            p->owned_pid = -1;
            p->started = false;
            return YAN_SEMANTIC_OK;
        }
        if (reaped == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) { p->owned_pid = -1; p->started = false; }
            return YAN_SEMANTIC_PROVIDER;
        }
        if (monotonic_ms() >= deadline) {
            break;
        }
        sleep_ms(20u);
    }
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) return YAN_SEMANTIC_PROVIDER;
    do { reaped = waitpid(pid, &status, 0); }
    while (reaped < 0 && errno == EINTR);
    if (reaped != pid && !(reaped < 0 && errno == ECHILD)) return YAN_SEMANTIC_PROVIDER;
    p->owned_pid = -1;
    p->started = false;
    return YAN_SEMANTIC_PROVIDER; /* Forced KILL is cleanup, not normal stop. */
}

/* --------------------------------------------------------- version child */

/* Runs `runner --version`, merging stdout and stderr into a bounded capture,
 * under the monotonic startup deadline and the borrowed cancel flag. Reaps
 * the child on every path. */
static YanSemanticResult capture_runner_version(YanSemanticLlama *p, char *out,
                                                size_t capacity,
                                                size_t *length_out)
{
    if (capacity == 0) return YAN_SEMANTIC_INVALID;
    out[0] = 0;
    *length_out = 0;
    int fds[2];
    if (pipe(fds) != 0) {
        return YAN_SEMANTIC_PROVIDER;
    }
    char **env = sanitized_env();
    if (env == NULL) {
        close(fds[0]);
        close(fds[1]);
        return YAN_SEMANTIC_NOSPACE;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        free(env);
        return YAN_SEMANTIC_PROVIDER;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0 ||
            dup2(fds[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(fds[1]);
        char *argv[3];
        argv[0] = (char *)p->runner;
        argv[1] = (char *)"--version";
        argv[2] = NULL;
        execve(p->runner, argv, env);
        _exit(127);
    }
    close(fds[1]);
    free(env);
    p->owned_pid = pid;

    const uint64_t deadline = monotonic_ms() +
                              (uint64_t)p->startup_timeout * 1000u;
    size_t used = 0u;
    bool eof = false;
    bool overflow = false;
    YanSemanticResult result = YAN_SEMANTIC_OK;
    while (!eof) {
        if (p->cancel != NULL && *p->cancel) {
            result = YAN_SEMANTIC_CANCELLED;
            break;
        }
        const uint64_t now = monotonic_ms();
        if (now >= deadline) {
            result = YAN_SEMANTIC_TIMEOUT;
            break;
        }
        uint64_t wait = deadline - now;
        if (wait > 50u) {
            wait = 50u;
        }
        struct pollfd watch;
        watch.fd = fds[0];
        watch.events = POLLIN;
        watch.revents = 0;
        const int ready = poll(&watch, 1u, (int)wait);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = YAN_SEMANTIC_PROVIDER;
            break;
        }
        if (ready == 0) {
            continue;
        }
        if ((watch.revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }
        char buffer[256];
        const ssize_t got = read(fds[0], buffer, sizeof buffer);
        if (got > 0) {
            size_t take = (size_t)got;
            if (used + take > capacity - 1u) {
                overflow = true;
                take = capacity - 1u - used;
            }
            memcpy(out + used, buffer, take);
            used += take;
        } else if (got == 0) {
            eof = true;
        } else if (errno != EINTR) {
            result = YAN_SEMANTIC_PROVIDER;
            break;
        }
    }
    close(fds[0]);
    if (result != YAN_SEMANTIC_OK) {
        stop_owned(p);
        *length_out = 0u;
        return result;
    }

    int status = 0;
    for (;;) {
        const pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            p->owned_pid = -1;
            break;
        }
        if (reaped == -1 && errno != EINTR) {
            if (errno == ECHILD) p->owned_pid = -1;
            stop_owned(p);
            return YAN_SEMANTIC_PROVIDER;
        }
        if (p->cancel != NULL && *p->cancel) {
            stop_owned(p);
            *length_out = 0u;
            return YAN_SEMANTIC_CANCELLED;
        }
        if (monotonic_ms() >= deadline) {
            stop_owned(p);
            *length_out = 0u;
            return YAN_SEMANTIC_TIMEOUT;
        }
        sleep_ms(20u);
    }
    out[used] = 0;
    *length_out = used;
    if (overflow || used == 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return YAN_SEMANTIC_PROVIDER;
    }
    return YAN_SEMANTIC_OK;
}

/* ----------------------------------------------------------- config hash */

static YanSemanticResult hash_tlv(YanSemanticSha256 *hasher, uint8_t tag,
                                  const void *bytes, uint32_t length)
{
    uint8_t header[5];
    header[0] = tag;
    header[1] = (uint8_t)length;
    header[2] = (uint8_t)(length >> 8);
    header[3] = (uint8_t)(length >> 16);
    header[4] = (uint8_t)(length >> 24);
    YanSemanticResult result = yan_semantic_sha256_update(hasher, header, 5u);
    if (result == YAN_SEMANTIC_OK && length != 0u) {
        result = yan_semantic_sha256_update(hasher, bytes, length);
    }
    return result;
}

static YanSemanticResult hash_u32(YanSemanticSha256 *hasher, uint8_t tag,
                                  uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
    return hash_tlv(hasher, tag, bytes, 4u);
}

/* ------------------------------------------------------------- embedding */

static YanSemanticResult parse_embedding(const uint8_t *data, size_t length,
                                         float out[YAN_SEMANTIC_DIMENSION])
{
    if (data == NULL || length == 0u) {
        return YAN_SEMANTIC_PROVIDER;
    }
    yyjson_doc *document = yyjson_read((const char *)data, length, 0);
    if (document == NULL) {
        return YAN_SEMANTIC_PROVIDER;
    }
    YanSemanticResult result = YAN_SEMANTIC_PROVIDER;
    yyjson_val *root = yyjson_doc_get_root(document);
    if (yyjson_is_obj(root)) {
        yyjson_val *array = yyjson_obj_get(root, "data");
        if (yyjson_is_arr(array) && yyjson_arr_size(array) == 1u) {
            yyjson_val *item = yyjson_arr_get_first(array);
            if (yyjson_is_obj(item)) {
                yyjson_val *index = yyjson_obj_get(item, "index");
                yyjson_val *embedding = yyjson_obj_get(item, "embedding");
                if (yyjson_is_int(index) && yyjson_get_int(index) == 0 &&
                    yyjson_is_arr(embedding) &&
                    yyjson_arr_size(embedding) ==
                        (size_t)YAN_SEMANTIC_DIMENSION) {
                    double norm = 0.0;
                    bool valid = true;
                    for (uint32_t i = 0u; i < YAN_SEMANTIC_DIMENSION; ++i) {
                        yyjson_val *value = yyjson_arr_get(embedding, i);
                        if (!yyjson_is_num(value)) {
                            valid = false;
                            break;
                        }
                        const double number = yyjson_get_num(value);
                        if (!(number <= FLT_MAX && number >= -FLT_MAX)) {
                            valid = false;
                            break;
                        }
                        const float converted = (float)number;
                        norm += (double)converted * (double)converted;
                    }
                    if (valid && norm > 0.0) {
                        for (uint32_t i = 0u; i < YAN_SEMANTIC_DIMENSION; ++i) {
                            out[i] = (float)yyjson_get_num(
                                yyjson_arr_get(embedding, i));
                        }
                        result = YAN_SEMANTIC_OK;
                    }
                }
            }
        }
    }
    yyjson_doc_free(document);
    return result;
}

/* ------------------------------------------------------------ operations */

static YanSemanticResult await_ready(YanSemanticLlama *p, uint64_t deadline)
{
    for (;;) {
        if (p->cancel != NULL && *p->cancel) {
            return YAN_SEMANTIC_CANCELLED;
        }
        if (!child_alive(p)) {
            return YAN_SEMANTIC_PROVIDER;
        }
        if (monotonic_ms() >= deadline) {
            return YAN_SEMANTIC_TIMEOUT;
        }
        long code = 0;
        const YanSemanticResult result = health_request(p, &code, deadline);
        if (result == YAN_SEMANTIC_OK) {
            if (code == 200) {
                return child_alive(p) ? YAN_SEMANTIC_OK : YAN_SEMANTIC_PROVIDER;
            }
            if (code == 401 || code == 403) {
                return YAN_SEMANTIC_PROVIDER;
            }
            if (code == 503 || code == 0) {
                sleep_ms(50u);
                continue;
            }
            return YAN_SEMANTIC_PROVIDER;
        }
        if (result == YAN_SEMANTIC_TIMEOUT) {
            if (monotonic_ms() >= deadline) {
                return YAN_SEMANTIC_TIMEOUT;
            }
            sleep_ms(20u);
            continue;
        }
        return result;
    }
}

static YanSemanticResult start(void *opaque)
{
    YanSemanticLlama *p = opaque;
    if (p == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    if (p->started || p->owned_pid > 0) {
        return YAN_SEMANTIC_INVALID;
    }
    if (p->cancel != NULL && *p->cancel) {
        return YAN_SEMANTIC_CANCELLED;
    }

    unsigned port = 0u;
    YanSemanticResult result = reserve_port(p->port, &port);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    if (!random_secret(p->secret)) {
        return YAN_SEMANTIC_PROVIDER;
    }
    snprintf(p->url, sizeof p->url, "http://127.0.0.1:%u", port);
    char port_text[6];
    snprintf(port_text, sizeof port_text, "%u", port);

    char **env = sanitized_env();
    if (env == NULL) {
        return YAN_SEMANTIC_NOSPACE;
    }
    char *argv[40];
    size_t count = 0u;
    argv[count++] = (char *)p->runner;
    argv[count++] = (char *)"--model";
    argv[count++] = (char *)p->model;
    argv[count++] = (char *)"--embedding";
    argv[count++] = (char *)"--pooling";
    argv[count++] = (char *)"last";
    argv[count++] = (char *)"--host";
    argv[count++] = (char *)"127.0.0.1";
    argv[count++] = (char *)"--port";
    argv[count++] = port_text;
    argv[count++] = (char *)"--api-key";
    argv[count++] = p->secret;
    argv[count++] = (char *)"--alias";
    argv[count++] = (char *)"yanos-embedding";
    argv[count++] = (char *)"--ctx-size";
    argv[count++] = (char *)"4096";
    argv[count++] = (char *)"--batch-size";
    argv[count++] = (char *)"4096";
    argv[count++] = (char *)"--ubatch-size";
    argv[count++] = (char *)"4096";
    argv[count++] = (char *)"--threads";
    argv[count++] = (char *)"4";
    argv[count++] = (char *)"--threads-batch";
    argv[count++] = (char *)"4";
    argv[count++] = (char *)"--n-gpu-layers";
    argv[count++] = (char *)"0";
    argv[count++] = (char *)"--parallel";
    argv[count++] = (char *)"1";
    argv[count++] = (char *)"--no-webui";
    argv[count++] = (char *)"--no-warmup";
    argv[count] = NULL;

    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd < 0) {
        free(env);
        return YAN_SEMANTIC_PROVIDER;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(null_fd);
        free(env);
        return YAN_SEMANTIC_PROVIDER;
    }
    if (pid == 0) {
        if (dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(null_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(null_fd);
        execve(p->runner, argv, env);
        _exit(127);
    }
    close(null_fd);
    free(env);
    p->owned_pid = pid;
    p->started = false;

    const uint64_t deadline = monotonic_ms() +
                              (uint64_t)p->startup_timeout * 1000u;
    result = await_ready(p, deadline);
    if (result != YAN_SEMANTIC_OK) {
        stop_owned(p);
        return result;
    }
    p->started = true;
    return YAN_SEMANTIC_OK;
}

static YanSemanticResult embed(void *opaque, const uint8_t *bytes, size_t length,
                               float out[YAN_SEMANTIC_DIMENSION])
{
    YanSemanticLlama *p = opaque;
    if (p == NULL || bytes == NULL || length == 0u || out == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    if (!p->started || p->owned_pid <= 0) {
        return YAN_SEMANTIC_INVALID;
    }
    if (p->cancel != NULL && *p->cancel) {
        return YAN_SEMANTIC_CANCELLED;
    }

    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    if (document == NULL) {
        return YAN_SEMANTIC_NOSPACE;
    }
    yyjson_mut_val *root = yyjson_mut_obj(document);
    if (root == NULL ||
        !yyjson_mut_obj_add_strn(document, root, "input", (const char *)bytes,
                                 length) ||
        !yyjson_mut_obj_add_str(document, root, "model", "yanos-embedding") ||
        !yyjson_mut_obj_add_str(document, root, "encoding_format", "float") ||
        !yyjson_mut_obj_add_bool(document, root, "truncate", false)) {
        yyjson_mut_doc_free(document);
        return YAN_SEMANTIC_NOSPACE;
    }
    yyjson_mut_doc_set_root(document, root);
    size_t json_length = 0u;
    char *json = yyjson_mut_write(document, 0, &json_length);
    yyjson_mut_doc_free(document);
    if (json == NULL) {
        return YAN_SEMANTIC_NOSPACE;
    }

    char url[128];
    char auth[128];
    snprintf(url, sizeof url, "%s/v1/embeddings", p->url);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", p->secret);
    struct curl_slist *headers = curl_slist_append(NULL, auth);
    if (headers == NULL) {
        free(json);
        return YAN_SEMANTIC_NOSPACE;
    }
    struct curl_slist *with_type = curl_slist_append(
        headers, "Content-Type: application/json");
    if (with_type == NULL) {
        curl_slist_free_all(headers);
        free(json);
        return YAN_SEMANTIC_NOSPACE;
    }
    headers = with_type;

    YanSemanticResponse response;
    memset(&response, 0, sizeof response);
    long code = 0;
    const uint64_t deadline = monotonic_ms() +
                              (uint64_t)p->request_timeout * 1000u;
    YanSemanticResult result = http_perform(p, url, headers, json, json_length,
                                            &response, &code, deadline);
    curl_slist_free_all(headers);
    free(json);
    if (result != YAN_SEMANTIC_OK) {
        free(response.data);
        return result;
    }
    if (code != 200) {
        free(response.data);
        return YAN_SEMANTIC_PROVIDER;
    }
    if (!child_alive(p)) {
        free(response.data);
        return YAN_SEMANTIC_PROVIDER;
    }
    result = parse_embedding(response.data, response.length, out);
    free(response.data);
    return result;
}

/* ------------------------------------------------------------ public API */

YanSemanticResult yan_semantic_llama_init(YanSemanticLlama *p, const char *model,
                                          const char *runner,
                                          unsigned startup_timeout,
                                          unsigned request_timeout,
                                          const volatile sig_atomic_t *cancel)
{
    if (p == NULL || model == NULL || runner == NULL || startup_timeout < 1u ||
        startup_timeout > 3600u || request_timeout < 1u ||
        request_timeout > 3600u) {
        return YAN_SEMANTIC_INVALID;
    }
    if (!curl_ready()) {
        return YAN_SEMANTIC_UNSUPPORTED;
    }
    if (p->initialized && (p->started || p->owned_pid > 0)) return YAN_SEMANTIC_BUSY;
    memset(p, 0, sizeof *p);
    p->model = model;
    p->runner = runner;
    p->startup_timeout = startup_timeout;
    p->request_timeout = request_timeout;
    p->cancel = cancel;
    p->owned_pid = -1;
    p->initialized = true;
    return YAN_SEMANTIC_OK;
}

YanSemanticResult yan_semantic_llama_config(YanSemanticLlama *p, uint8_t hash[32])
{
    if (p == NULL || hash == NULL || !p->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (p->started || p->owned_pid > 0) return YAN_SEMANTIC_BUSY;
    if (p->cancel != NULL && *p->cancel) {
        return YAN_SEMANTIC_CANCELLED;
    }
    char version[1024];
    size_t version_length = 0u;
    YanSemanticResult result = capture_runner_version(p, version,
                                                      sizeof version,
                                                      &version_length);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    memcpy(p->version, version, version_length);
    p->version[version_length] = 0;

    YanSemanticSha256 hasher;
    result = yan_semantic_sha256_init(&hasher);
    if (result == YAN_SEMANTIC_OK) {
        result = hash_u32(&hasher, 1u, 1u);
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_u32(&hasher, 2u, 1u);
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_u32(&hasher, 3u, 1u);
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_u32(&hasher, 4u, 1u);
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_u32(&hasher, 5u, YAN_SEMANTIC_DIMENSION);
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_tlv(&hasher, 6u, yan_quantization,
                          (uint32_t)(sizeof yan_quantization - 1u));
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_tlv(&hasher, 7u, yan_pooling,
                          (uint32_t)(sizeof yan_pooling - 1u));
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_tlv(&hasher, 8u, yan_query_prefix,
                          (uint32_t)(sizeof yan_query_prefix - 1u));
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_tlv(&hasher, 9u, yan_title_prefix,
                          (uint32_t)(sizeof yan_title_prefix - 1u));
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_tlv(&hasher, 10u, yan_title_separator,
                          (uint32_t)(sizeof yan_title_separator - 1u));
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_tlv(&hasher, 11u, yan_inference_flags,
                          (uint32_t)(sizeof yan_inference_flags - 1u));
    }
    if (result == YAN_SEMANTIC_OK) {
        result = hash_tlv(&hasher, 12u, p->version, (uint32_t)version_length);
    }
    if (result == YAN_SEMANTIC_OK) {
        result = yan_semantic_sha256_final(&hasher, hash);
    }
    return result;
}

YanSemanticResult yan_semantic_llama_stop(void *opaque)
{
    YanSemanticLlama *p = opaque;
    if (p == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    return stop_owned(p);
}

void yan_semantic_llama_bind(YanSemanticLlama *p, YanSemanticProvider *out)
{
    *out = (YanSemanticProvider){p, sizeof *p, start, embed,
                                 yan_semantic_llama_stop};
}
