/* Test-only link wrapper: exercise the production app's RELATION_INIT failure. */
#include "note_relations.h"
YanNoteResult __wrap_yan_relation_init(YanRelationContext *context, YanFs *fs);
YanNoteResult __wrap_yan_relation_init(YanRelationContext *context, YanFs *fs)
{
    (void)context;
    (void)fs;
    return YAN_NOTE_INVALID;
}
