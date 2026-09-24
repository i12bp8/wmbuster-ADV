// wM-Buster ADV — formula evaluator for driver `calculate` expressions.
// Values carry units: literals like 1m3 / 256counter / 1 month / 0.5y, field
// references like total_m3 or meter_datetime (converted to the requested
// unit), C-like operators (arithmetic, bitwise, comparison, logical) and
// sqrt/floor/ceil/round/abs/min/max/pow. Points in time are seconds since the
// unix epoch; adding months/years is calendar correct (as upstream).
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/types.h"

namespace wmb {

struct FVal {
    double v;
    Unit   u;          // Unknown = plain number
    bool   composite;  // product/quotient of units that we do not track
    bool   ok;
};

// Resolve an identifier (e.g. "prev_raw_counter", "storage_counter").
typedef bool (*FResolver)(const char* ident, FVal* out, void* ctx);

FVal formula_eval(const char* expr, FResolver resolver, void* ctx);

// Parse a unit suffix ("kwh", "m3", "counter", ...). Unknown when not a unit.
Unit unit_from_suffix(const char* s, size_t len);

// Calendar helper shared with the engine: add months to a unix time.
double add_months(double t, int months);

} // namespace wmb
