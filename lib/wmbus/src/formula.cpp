// wM-Buster ADV — unit aware formula evaluator.
// GPL-3.0
#include "wmbus/formula.h"
#include "wmbus/dv.h"

#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace wmb {

Unit unit_from_suffix(const char* s, size_t len) {
#define X(cname, lcname, hr, q) if (len == sizeof(#lcname) - 1 && strncmp(s, #lcname, len) == 0) return Unit::cname;
    WMB_LIST_OF_UNITS
#undef X
    if (len == 6 && strncmp(s, "months", 6) == 0) return Unit::Month;
    return Unit::Unknown;
}

// ---------------------------------------------------------------------------
// Calendar month arithmetic (util.cc addMonths): the last day of a month maps
// to the last day of the resulting month, other days are clamped.
// ---------------------------------------------------------------------------
static bool leap_upstream(int tm_year) {
    // Upstream passes tm_year (years since 1900) to is_leap_year().
    return ((tm_year % 4 == 0) && (tm_year % 100 != 0)) || (tm_year % 400 == 0);
}

static int days_in_month_upstream(int tm_year, int month0) {
    static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month0 < 0 || month0 >= 12) month0 = 0;
    int d = D[month0];
    if (month0 == 1 && leap_upstream(tm_year)) d++;
    return d;
}

double add_months(double t, int months) {
    int y, mo, d, h, mi, s;
    unix_to_civil(t, &y, &mo, &d, &h, &mi, &s);
    int tm_year = y - 1900, tm_mon = mo - 1;
    bool last = d == days_in_month_upstream(tm_year, tm_mon);
    int year = tm_year + months / 12;
    int month = tm_mon + months % 12;
    while (month > 11) { year++; month -= 12; }
    while (month < 0) { year--; month += 12; }
    int dim = days_in_month_upstream(year, month);
    int day = last ? dim : (d < dim ? d : dim);
    return civil_to_unix(year + 1900, month + 1, day, h, mi, s);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
struct P {
    const char* p;
    FResolver resolver;
    void* ctx;
};

static FVal mk(double v, Unit u = Unit::Unknown) { return FVal{ v, u, false, true }; }
static FVal bad() { return FVal{ 0, Unit::Unknown, false, false }; }

static bool is_scalar(const FVal& a) {
    if (a.composite) return false;
    return a.u == Unit::Unknown || a.u == Unit::COUNTER || a.u == Unit::FACTOR || a.u == Unit::NUMBER;
}

static bool is_time_point(const FVal& a) { return !a.composite && unit_quantity(a.u) == Quantity::PointInTime; }

static void ws(P* ps) { while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++; }

static FVal parse_expr(P* ps);

static FVal op_add(FVal a, FVal b, int sign) {
    if (!a.ok || !b.ok) return bad();
    if (is_time_point(a) && !is_time_point(b)) {
        if (b.u == Unit::Month) return mk(add_months(a.v, (int)(sign * b.v)), a.u);
        if (b.u == Unit::Year) return mk(add_months(a.v, (int)(sign * b.v * 12)), a.u);
        double secs = unit_quantity(b.u) == Quantity::Time ? unit_convert(b.v, b.u, Unit::Second) : b.v;
        return mk(a.v + sign * secs, a.u);
    }
    if (is_time_point(b) && !is_time_point(a) && sign > 0) {
        if (a.u == Unit::Month) return mk(add_months(b.v, (int)a.v), b.u);
        if (a.u == Unit::Year) return mk(add_months(b.v, (int)(a.v * 12)), b.u);
        double secs = unit_quantity(a.u) == Quantity::Time ? unit_convert(a.v, a.u, Unit::Second) : a.v;
        return mk(b.v + secs, b.u);
    }
    if (is_time_point(a) && is_time_point(b)) {
        if (sign < 0) return mk(a.v - b.v, Unit::Second);
        return mk(a.v + b.v, a.u);
    }
    if (a.composite || b.composite) {
        FVal r = mk(a.v + sign * b.v);
        r.composite = true;
        return r;
    }
    if (a.u == b.u) return mk(a.v + sign * b.v, a.u);
    if (is_scalar(a) && !is_scalar(b)) return mk(a.v + sign * b.v, b.u);
    if (is_scalar(b)) return mk(a.v + sign * b.v, a.u);
    if (units_convertible(b.u, a.u)) return mk(a.v + sign * unit_convert(b.v, b.u, a.u), a.u);
    FVal r = mk(a.v + sign * b.v);
    r.composite = true;
    return r;
}

static FVal op_mul(FVal a, FVal b) {
    if (!a.ok || !b.ok) return bad();
    if (is_scalar(a) && is_scalar(b)) {
        Unit u = (a.u == Unit::COUNTER || b.u == Unit::COUNTER) ? Unit::COUNTER : (a.u != Unit::Unknown ? a.u : b.u);
        return mk(a.v * b.v, u);
    }
    if (is_scalar(a)) { FVal r = b; r.v = a.v * b.v; return r; }
    if (is_scalar(b)) { FVal r = a; r.v = a.v * b.v; return r; }
    FVal r = mk(a.v * b.v);
    r.composite = true;
    return r;
}

static FVal op_div(FVal a, FVal b) {
    if (!a.ok || !b.ok) return bad();
    double bv = b.v;
    if (bv == 0) return mk(0, a.u);
    if (is_scalar(b)) { FVal r = a; r.v = a.v / bv; return r; }
    if (!a.composite && !b.composite) {
        if (a.u == b.u) return mk(a.v / bv, Unit::COUNTER);
        if (units_convertible(b.u, a.u)) return mk(a.v / unit_convert(b.v, b.u, a.u), Unit::COUNTER);
    }
    FVal r = mk(a.v / bv);
    r.composite = true;
    return r;
}

static FVal with_value(FVal a, double v) { a.v = v; return a; }

static double align(const FVal& a, const FVal& b) {
    if (!a.composite && !b.composite && a.u != b.u && units_convertible(b.u, a.u)) return unit_convert(b.v, b.u, a.u);
    return b.v;
}

static FVal parse_primary(P* ps) {
    ws(ps);
    char c = *ps->p;
    if (c == '(') {
        ps->p++;
        FVal v = parse_expr(ps);
        ws(ps);
        if (*ps->p != ')') return bad();
        ps->p++;
        return v;
    }
    if (c == '"' || c == '\'') {
        // Date literal "YYYY-MM-DD[ HH:MM[:SS]]"
        char q = c;
        ps->p++;
        int y = 0, m = 0, d = 0, h = 0, mi = 0, s = 0;
        const char* start = ps->p;
        while (*ps->p && *ps->p != q) ps->p++;
        if (*ps->p != q) return bad();
        char buf[32];
        size_t n = (size_t)(ps->p - start);
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, start, n);
        buf[n] = 0;
        ps->p++;
        int k = sscanf(buf, "%d-%d-%d %d:%d:%d", &y, &m, &d, &h, &mi, &s);
        if (k < 3) return bad();
        return mk(civil_to_unix(y, m, d, h, mi, s), k > 3 ? Unit::DateTimeLT : Unit::DateLT);
    }
    if (isdigit((unsigned char)c) || c == '.') {
        const char* start = ps->p;
        while (isdigit((unsigned char)*ps->p)) ps->p++;
        if (*ps->p == '.') { ps->p++; while (isdigit((unsigned char)*ps->p)) ps->p++; }
        double v = strtod(start, nullptr);
        // Optional unit, possibly separated by whitespace.
        const char* save = ps->p;
        ws(ps);
        const char* us = ps->p;
        while (isalnum((unsigned char)*ps->p)) ps->p++;
        size_t ul = (size_t)(ps->p - us);
        if (ul > 0) {
            Unit u = unit_from_suffix(us, ul);
            if (u != Unit::Unknown) return mk(v, u);
        }
        ps->p = save;
        return mk(v);
    }
    if (isalpha((unsigned char)c) || c == '_') {
        char ident[64];
        size_t n = 0;
        while ((isalnum((unsigned char)*ps->p) || *ps->p == '_') && n < sizeof(ident) - 1) ident[n++] = *ps->p++;
        ident[n] = 0;
        ws(ps);
        if (*ps->p == '(') {
            ps->p++;
            FVal args[3];
            int na = 0;
            ws(ps);
            if (*ps->p != ')') {
                for (;;) {
                    if (na >= 3) return bad();
                    args[na++] = parse_expr(ps);
                    ws(ps);
                    if (*ps->p == ',') { ps->p++; continue; }
                    break;
                }
            }
            ws(ps);
            if (*ps->p != ')') return bad();
            ps->p++;
            for (int i = 0; i < na; ++i) if (!args[i].ok) return bad();
            if (!strcmp(ident, "sqrt") && na == 1) {
                FVal r = with_value(args[0], sqrt(args[0].v));
                if (!is_scalar(args[0])) { r.composite = true; r.u = Unit::Unknown; }
                return r;
            }
            if (!strcmp(ident, "abs") && na == 1) return with_value(args[0], fabs(args[0].v));
            if (!strcmp(ident, "floor") && na == 1) return with_value(args[0], floor(args[0].v));
            if (!strcmp(ident, "ceil") && na == 1) return with_value(args[0], ceil(args[0].v));
            if (!strcmp(ident, "round") && na == 1) return with_value(args[0], round(args[0].v));
            if (!strcmp(ident, "min") && na == 2) return with_value(args[0], fmin(args[0].v, align(args[0], args[1])));
            if (!strcmp(ident, "max") && na == 2) return with_value(args[0], fmax(args[0].v, align(args[0], args[1])));
            if (!strcmp(ident, "pow") && na == 2) return with_value(args[0], pow(args[0].v, args[1].v));
            if (!strcmp(ident, "mkdate") && na == 3) {
                // mkdate(year, month, day) -> point in time (midnight).
                long long y = llround(args[0].v), mo = llround(args[1].v), d = llround(args[2].v);
                if (mo <= 0 || d <= 0) return mk(NAN, Unit::UnixTimestamp);
                return mk(civil_to_unix((int)y, (int)mo, (int)d, 0, 0, 0), Unit::UnixTimestamp);
            }
            return bad();
        }
        if (!strcmp(ident, "true")) return mk(1, Unit::COUNTER);
        if (!strcmp(ident, "false")) return mk(0, Unit::COUNTER);
        FVal out;
        if (ps->resolver && ps->resolver(ident, &out, ps->ctx)) return out;
        return bad();
    }
    return bad();
}

static FVal parse_unary(P* ps) {
    ws(ps);
    char c = *ps->p;
    if (c == '-') { ps->p++; FVal v = parse_unary(ps); v.v = -v.v; return v; }
    if (c == '+') { ps->p++; return parse_unary(ps); }
    if (c == '!') { ps->p++; FVal v = parse_unary(ps); return v.ok ? mk(v.v == 0 ? 1 : 0, Unit::COUNTER) : bad(); }
    if (c == '~') { ps->p++; FVal v = parse_unary(ps); return with_value(v, (double)~(int64_t)v.v); }
    return parse_primary(ps);
}

static FVal parse_mul(P* ps) {
    FVal a = parse_unary(ps);
    for (;;) {
        ws(ps);
        char c = *ps->p;
        if (c != '*' && c != '/' && c != '%') return a;
        ps->p++;
        FVal b = parse_unary(ps);
        if (!a.ok || !b.ok) return bad();
        if (c == '*') a = op_mul(a, b);
        else if (c == '/') a = op_div(a, b);
        else {
            int64_t bb = (int64_t)align(a, b);
            a = with_value(a, bb == 0 ? 0 : (double)((int64_t)a.v % bb));
        }
    }
}

static FVal parse_add(P* ps) {
    FVal a = parse_mul(ps);
    for (;;) {
        ws(ps);
        char c = *ps->p;
        if (c != '+' && c != '-') return a;
        ps->p++;
        FVal b = parse_mul(ps);
        a = op_add(a, b, c == '+' ? 1 : -1);
    }
}

static FVal parse_shift(P* ps) {
    FVal a = parse_add(ps);
    for (;;) {
        ws(ps);
        if (ps->p[0] == '<' && ps->p[1] == '<') {
            ps->p += 2;
            FVal b = parse_add(ps);
            if (!a.ok || !b.ok) return bad();
            a = with_value(a, (double)((int64_t)a.v << (int)b.v));
        } else if (ps->p[0] == '>' && ps->p[1] == '>') {
            ps->p += 2;
            FVal b = parse_add(ps);
            if (!a.ok || !b.ok) return bad();
            a = with_value(a, (double)((int64_t)a.v >> (int)b.v));
        } else {
            return a;
        }
    }
}

static FVal parse_rel(P* ps) {
    FVal a = parse_shift(ps);
    for (;;) {
        ws(ps);
        const char* s = ps->p;
        int op = 0;
        if (s[0] == '<' && s[1] == '=') { op = 1; ps->p += 2; }
        else if (s[0] == '>' && s[1] == '=') { op = 2; ps->p += 2; }
        else if (s[0] == '<' && s[1] != '<') { op = 3; ps->p += 1; }
        else if (s[0] == '>' && s[1] != '>') { op = 4; ps->p += 1; }
        else return a;
        FVal b = parse_shift(ps);
        if (!a.ok || !b.ok) return bad();
        double bv = align(a, b);
        bool r = op == 1 ? a.v <= bv : op == 2 ? a.v >= bv : op == 3 ? a.v < bv : a.v > bv;
        a = mk(r ? 1 : 0, Unit::COUNTER);
    }
}

static FVal parse_eq(P* ps) {
    FVal a = parse_rel(ps);
    for (;;) {
        ws(ps);
        bool eq;
        if (ps->p[0] == '=' && ps->p[1] == '=') { eq = true; ps->p += 2; }
        else if (ps->p[0] == '!' && ps->p[1] == '=') { eq = false; ps->p += 2; }
        else return a;
        FVal b = parse_rel(ps);
        if (!a.ok || !b.ok) return bad();
        double bv = align(a, b);
        a = mk(((a.v == bv) == eq) ? 1 : 0, Unit::COUNTER);
    }
}

static FVal parse_band(P* ps) {
    FVal a = parse_eq(ps);
    for (;;) {
        ws(ps);
        if (ps->p[0] == '&' && ps->p[1] != '&') {
            ps->p++;
            FVal b = parse_eq(ps);
            if (!a.ok || !b.ok) return bad();
            a = with_value(a, (double)((int64_t)a.v & (int64_t)b.v));
        } else return a;
    }
}

static FVal parse_bxor(P* ps) {
    FVal a = parse_band(ps);
    for (;;) {
        ws(ps);
        if (ps->p[0] == '^') {
            ps->p++;
            FVal b = parse_band(ps);
            if (!a.ok || !b.ok) return bad();
            a = with_value(a, (double)((int64_t)a.v ^ (int64_t)b.v));
        } else return a;
    }
}

static FVal parse_bor(P* ps) {
    FVal a = parse_bxor(ps);
    for (;;) {
        ws(ps);
        if (ps->p[0] == '|' && ps->p[1] != '|') {
            ps->p++;
            FVal b = parse_bxor(ps);
            if (!a.ok || !b.ok) return bad();
            a = with_value(a, (double)((int64_t)a.v | (int64_t)b.v));
        } else return a;
    }
}

static FVal parse_land(P* ps) {
    FVal a = parse_bor(ps);
    for (;;) {
        ws(ps);
        if (ps->p[0] == '&' && ps->p[1] == '&') {
            ps->p += 2;
            FVal b = parse_bor(ps);
            if (!a.ok || !b.ok) return bad();
            a = mk((a.v != 0 && b.v != 0) ? 1 : 0, Unit::COUNTER);
        } else return a;
    }
}

static FVal parse_lor(P* ps) {
    FVal a = parse_land(ps);
    for (;;) {
        ws(ps);
        if (ps->p[0] == '|' && ps->p[1] == '|') {
            ps->p += 2;
            FVal b = parse_land(ps);
            if (!a.ok || !b.ok) return bad();
            a = mk((a.v != 0 || b.v != 0) ? 1 : 0, Unit::COUNTER);
        } else return a;
    }
}

static FVal parse_expr(P* ps) { return parse_lor(ps); }

FVal formula_eval(const char* expr, FResolver resolver, void* ctx) {
    P ps{ expr, resolver, ctx };
    FVal v = parse_expr(&ps);
    ws(&ps);
    if (*ps.p != 0) return bad();
    return v;
}

} // namespace wmb
