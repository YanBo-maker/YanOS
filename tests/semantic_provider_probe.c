#define _POSIX_C_SOURCE 200809L
#include "semantic_llama.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

static volatile sig_atomic_t cancelled;
static void cancel(int number) { (void)number; cancelled = 1; }
int main(int argc, char **argv)
{
    if (argc != 6) return 1;
    struct sigaction action;
    memset(&action, 0, sizeof action); action.sa_handler = cancel;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return 1;
    YanSemanticLlama llama = {0};
    YanSemanticResult r = yan_semantic_llama_init(&llama, argv[1], argv[2],
        1, 1, &cancelled);
    llama.port = (unsigned)strtoul(argv[3], NULL, 10);
    if (strcmp(argv[4], "cancel") == 0) cancelled = 1;
    uint8_t config[32];
    if (r == YAN_SEMANTIC_OK) r = yan_semantic_llama_config(&llama, config);
    YanSemanticProvider provider;
    yan_semantic_llama_bind(&llama, &provider);
    if (r == YAN_SEMANTIC_OK && strcmp(argv[5], "version") != 0) r = provider.start(provider.context);
    float vector[1024];
    if (r == YAN_SEMANTIC_OK && (strcmp(argv[5], "embed") == 0 || strcmp(argv[5], "exit_stop") == 0))
        r = provider.embed(provider.context, (const uint8_t *)"hello\xE4\xB8\xAD", 8, vector);
    pid_t owned = llama.owned_pid;
    if (strcmp(argv[5], "exit_stop") == 0) {
        struct timespec delay = {0, 200000000};
        nanosleep(&delay, NULL);
    }
    YanSemanticResult stopped = provider.stop(provider.context);
    if (r == YAN_SEMANTIC_OK) r = stopped;
    bool reaped = owned <= 0;
    if (owned > 0) { errno = 0; reaped = waitpid(owned, NULL, WNOHANG) == -1 && errno == ECHILD; }
    printf("RESULT %s reaped=%d\n", yan_semantic_result_name(r), reaped ? 1 : 0);
    return !reaped || ferror(stdout) ? 1 : 0;
}
