#ifndef YAN_HOST_SEMANTIC_LLAMA_H
#define YAN_HOST_SEMANTIC_LLAMA_H

#include "semantic.h"
#include <signal.h>
#include <sys/types.h>

/* Optional Linux HTTP/process adapter; libcurl and yyjson are private deps.
 * Zero-initialize before first init; re-init/config while a child is owned is
 * BUSY and preserves ownership. The caller always calls stop before release.
 * Each start owns one fork/exec child, random authentication and loopback port.
 * All operations have monotonic deadlines. stop is idempotent and reaps only
 * owned_pid. cancel is a borrowed signal-safe flag, never a global service.
 * port=0 selects an ephemeral port; explicit port supports collision tests.
 * No automatic download, remote endpoint, proxy or redirect is supported. */
typedef struct {
    const char *model, *runner;
    unsigned startup_timeout, request_timeout, port;
    const volatile sig_atomic_t *cancel;
    pid_t owned_pid;
    bool initialized, started;
    char secret[65], url[96];
    char version[1024];
} YanSemanticLlama;

YanSemanticResult yan_semantic_llama_init(YanSemanticLlama *, const char *model,
    const char *runner, unsigned startup_timeout, unsigned request_timeout,
    const volatile sig_atomic_t *cancel);
/* Executes runner --version with a bounded pipe, reaps it, and hashes exact
 * preprocessing versions/prefixes/dimension/pooling/quant/fixed inference flags
 * with explicit lengths. No model is loaded by this operation. */
YanSemanticResult yan_semantic_llama_config(YanSemanticLlama *, uint8_t hash[32]);
void yan_semantic_llama_bind(YanSemanticLlama *, YanSemanticProvider *);
YanSemanticResult yan_semantic_llama_stop(void *);

#endif
