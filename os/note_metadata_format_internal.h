#ifndef YAN_OS_NOTE_METADATA_FORMAT_INTERNAL_H
#define YAN_OS_NOTE_METADATA_FORMAT_INTERNAL_H

#include "note_metadata_format.h"

/* Same bounded grammar as the full parser, with only its body origin
 * published. Errors preserve *offset; input/workspace/holder are disjoint. */
YanNoteResult yan_note_body_offset(const uint8_t *bytes, uint32_t length,
                                   uint32_t *offset,
                                   YanNoteParseWorkspace *workspace);

#endif
