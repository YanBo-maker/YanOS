/* yan_graph: the 0044 R2 read-only knowledge-graph command.
 *
 * Contract: docs/specs/0044-evidence-knowledge-graph.md R2 and its
 * "R2接口字段锁定", plus docs/decisions/0008-evidence-knowledge-graph.md.
 *
 *   yan_graph --disk-image IMAGE list
 *   yan_graph --disk-image IMAGE query TEXT
 *
 * This command is a thin front end: it parses the two fixed forms, ignores
 * SIGPIPE and writes one line. The query lifecycle and the JSON object come from
 * host/knowledge_graph_query.c, which is the single implementation shared with
 * the MCP graph worker (0045 R1). Consequently the command keeps no model,
 * runner, CURL or yyjson dependency, and the 0044 field order, page caps, row
 * order and error tokens are unchanged because that module is where they live.
 *
 * Order of work, which is what makes the failure contract true:
 *   parse arguments (no I/O at all) -> the service validates again, then opens
 *   and mounts the image, observes digest A, compares the optional expected
 *   source, builds, selects, closes the build source (a close error outranks OK),
 *   re-opens and observes digest B, compares A/B (STALE), and encodes the whole
 *   JSON into memory -> this command writes that buffer with one fwrite, one
 *   fputc and one fflush. Therefore every ordinary error leaves stdout empty,
 *   and only a real stdout failure can leave a prefix behind. The caller must
 *   check both the exit status and that the whole object arrived.
 *
 * The graph query itself never scans: concepts come from the immutable ID-ordered
 * array, and propositions come from the concept's own out/in/self adjacency
 * slices, merged by the canonical (file name, declaration byte) order. */

#define _POSIX_C_SOURCE 200809L

#include "knowledge_graph_query.h"
#include "semantic_text.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ argument parsing */

/* The failure contract is one diagnostic line, so the reminder of the accepted
 * forms is appended after the ERROR token rather than replacing it. */
static void usage(void)
{
    fputs("ERROR USAGE\n", stderr);
    fputs("usage: yan_graph --disk-image IMAGE list\n", stderr);
    fputs("       yan_graph --disk-image IMAGE query TEXT\n", stderr);
    fputs("  list        concepts in ID order, first 20 of total\n", stderr);
    fputs("  query TEXT  exact concept ID, else a unique name or alias\n", stderr);
    fputs("              (1..256 bytes, single line, valid UTF-8)\n", stderr);
}

/* Exactly the two advertised forms are accepted:
 *
 *   yan_graph --disk-image IMAGE list
 *   yan_graph --disk-image IMAGE query TEXT
 *
 * The positions are fixed, so TEXT is data by construction. A concept ID may
 * legally begin with `-` (the ID alphabet includes it), so a query argument that
 * spells `-x` or even `--disk-image` must reach the graph as a lookup text; the
 * option parser must never inspect it. Only argv[1] is ever compared with
 * `--disk-image`, which is the one place an option can appear, and an unknown
 * spelling there stays USAGE.
 *
 * Nothing here opens, stats or reads the image, so a usage error performs zero
 * I/O. The query text is checked here with the same shared 0029 single-line
 * qualification and 1..256 byte bound the 0044 command always used, so a text
 * that fails it stays USAGE/exit 2 rather than becoming a runtime error; the
 * service re-applies both rules so a second caller cannot skip them. */
static bool options_parse(int argc, char **argv, YanKgQueryOptions *query)
{
    memset(query, 0, sizeof *query);
    query->operation = YAN_KG_QUERY_LIST;
    if (argc < 4 || strcmp(argv[1], "--disk-image") != 0) {
        return false;
    }
    if (argv[2][0] == '\0') {
        return false;
    }
    query->image = argv[2];
    if (strcmp(argv[3], "list") == 0) {
        return argc == 4;
    }
    if (strcmp(argv[3], "query") != 0 || argc != 5) {
        return false;
    }
    /* argv[4] is the query text, whatever it looks like. */
    size_t length = strlen(argv[4]);
    if (length == 0u || length > YAN_KG_QUERY_MAX ||
        !yan_semantic_query_valid((const uint8_t *)argv[4], length)) {
        return false;
    }
    query->operation = YAN_KG_QUERY_LOOKUP;
    query->query = argv[4];
    query->query_length = (uint32_t)length;
    return true;
}

static int fail(YanKgResult result)
{
    fprintf(stderr, "ERROR %s\n", yan_kg_result_name(result));
    return 1;
}

int main(int argc, char **argv)
{
    YanKgQueryOptions query;
    if (!options_parse(argc, argv, &query)) {
        usage();
        return 2;
    }
    /* A closed stdout must be an error at write time, not a signal that kills
     * the process in the middle of the object. */
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) != 0) {
        return fail(YAN_KG_IO);
    }

    /* A 0044 invocation sets no expected source, no cancellation source and no
     * size cap; the service reads 0 as unlimited output. */
    char *payload = NULL;
    size_t length = 0u;
    YanKgResult result = yan_kg_query_json(&query, &payload, &length);
    if (result != YAN_KG_OK) {
        free(payload);
        return fail(result);
    }
    bool written = fwrite(payload, 1u, length, stdout) == length;
    bool newline = fputc('\n', stdout) != EOF;
    bool flushed = fflush(stdout) == 0;
    free(payload);
    if (!written || !newline || !flushed) {
        return fail(YAN_KG_IO);
    }
    return 0;
}
