/* Test-only link wrapper: exercise the production app's RELATION_INIT failure. */
#include "note_relations.h"
YanNoteResult __wrap_yan_relation_init(YanRelationContext *context, YanFs *fs, YanRelationRow *rows, uint16_t *sort_index);
YanNoteResult __wrap_yan_relation_init(YanRelationContext *context, YanFs *fs, YanRelationRow *rows, uint16_t *sort_index)
{
    (void)context;
    (void)fs;
    (void)rows;
    (void)sort_index;
    return YAN_NOTE_INVALID;
}
