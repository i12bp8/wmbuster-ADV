// wM-Buster ADV — lookup translation of status/error bits into flag names
// (port of wmbusmeters translatebits.cc) and status string helpers.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/driver_table.h"

namespace wmb {

// Translate bits with all rules; result is sorted/deduplicated like upstream.
void lookup_translate(const LookupRule* rules, int num_rules, uint64_t bits, char* out, size_t out_max);

// Sort and de-duplicate space separated flags ('~' becomes ' ').
void status_sort(char* s);
// joinStatusOKStrings / joinStatusEmptyStrings
void status_join_ok(const char* a, const char* b, char* out, size_t out_max);
void status_join_empty(const char* a, const char* b, char* out, size_t out_max);

// TPL status byte, standard bits only (e.g. "BUSY", "ERROR", "POWER_LOW").
void tpl_status_standard(uint8_t sts, char* out, size_t out_max);

} // namespace wmb
