/* Linked only into the test CLI clone. Production main is compiled unchanged
 * except for this allocator name, to exercise its real collection failure. */
#undef calloc
#include <stdlib.h>
#include "semantic_json.h"
void *yan_semantic_test_calloc(size_t count, size_t size)
{
    if (count == 1 && size == sizeof(YanSemanticJsonRows)) return NULL;
    return calloc(count, size);
}
