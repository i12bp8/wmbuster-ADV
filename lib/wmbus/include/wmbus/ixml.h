// wM-Buster ADV — tiny ixml runtime for manufacturer specific payloads.
//
// Upstream wmbusmeters drivers describe proprietary payload layouts with
// Invisible XML grammars over the payload hex string; elements carrying a
// @dvk attribute become synthetic DV entries (dvk + captured hex). The
// generator compiles those grammars into node tables (driver_table.h) and
// this backtracking matcher executes them on-device.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/driver_table.h"

namespace wmb {

enum IxmlNodeType : uint8_t {
    IX_SEQ = 0,    // children[a .. a+b)
    IX_ALT = 1,    // children[a .. a+b)
    IX_REP = 2,    // child children[a], min b, max c (0xFFFF = unbounded)
    IX_LIT = 3,    // literals[a .. a+b)
    IX_CLASS = 4,  // literals[a .. a+b) holds lo/hi pairs; matches c chars (default 1)
    IX_REF = 5,    // rule a (capturing element)
    IX_EMPTY = 6,  // matches nothing (insertions / attributes)
};

#define IXF_HIDDEN 0x01   // hidden literal: text not part of element content

struct IxmlCapture {
    const char* dvk;
    uint16_t start;      // char offset in the input
    uint16_t len;        // number of chars in text
    char text[48];       // captured text (hidden parts removed)
};

#define WMB_IXML_MAX_CAPTURES 32

struct IxmlResult {
    IxmlCapture caps[WMB_IXML_MAX_CAPTURES];
    int n;
};

// Match the whole hex string (upper case) against the grammar.
bool ixml_match(const IxmlGrammar* g, const char* input, size_t len, IxmlResult* out);

} // namespace wmb
