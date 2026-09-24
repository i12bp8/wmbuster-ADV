// wM-Buster ADV — lookup translation (port of wmbusmeters translatebits.cc).
// GPL-3.0
#include "wmbus/lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace wmb {

struct SBuf {
    char* p;
    size_t cap;
    size_t len;
    void add(const char* s) {
        size_t n = strlen(s);
        if (len + n + 1 > cap) n = cap > len + 1 ? cap - len - 1 : 0;
        memcpy(p + len, s, n);
        len += n;
        p[len] = 0;
    }
};

static void trim_right(char* s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}

static uint64_t rule_mask(const LookupRule* r) {
    if (r->mask != 0) return r->mask;
    uint64_t m = 0;
    for (int i = 0; i < r->num_maps; ++i) m |= r->maps[i].value;
    return m;
}

static void handle_bits(const LookupRule* r, uint64_t bits, SBuf* s) {
    uint64_t mask = rule_mask(r);
    char tmp[96];
    bits &= mask;
    for (int i = 0; i < r->num_maps; ++i) {
        const LookupMap* m = &r->maps[i];
        if ((~mask & m->value) != 0) {
            snprintf(tmp, sizeof(tmp), "BAD_RULE_%s(from=0x%x mask=0x%x) ", r->name, (unsigned)m->value, (unsigned)mask);
            s->add(tmp);
        }
        uint64_t from = m->value & mask;
        if (m->test == TestBit::Set) {
            if ((bits & from) != 0) {
                s->add(m->name);
                s->add(" ");
                bits &= ~m->value;
            }
        } else {
            if ((bits & from) == 0) {
                s->add(m->name);
                s->add(" ");
            } else {
                bits &= ~m->value;
            }
        }
    }
    if (bits != 0) {
        snprintf(tmp, sizeof(tmp), "%s_%llX ", r->name, (unsigned long long)bits);
        s->add(tmp);
    }
    if (s->len == 0) {
        s->add(r->default_message ? r->default_message : "");
        s->add(" ");
    }
}

static void handle_index(const LookupRule* r, uint64_t bits, SBuf* s) {
    uint64_t mask = rule_mask(r);
    char tmp[96];
    bits &= mask;
    bool found = false;
    for (int i = 0; i < r->num_maps; ++i) {
        const LookupMap* m = &r->maps[i];
        if ((~mask & m->value) != 0) {
            snprintf(tmp, sizeof(tmp), "BAD_RULE_%s(from=0x%x mask=0x%x) ", r->name, (unsigned)m->value, (unsigned)r->mask);
            s->add(tmp);
        }
        uint64_t from = m->value & mask;
        if (bits == from) {
            s->add(m->name);
            s->add(" ");
            found = true;
        }
    }
    if (!found) {
        snprintf(tmp, sizeof(tmp), "%s_%llX ", r->name, (unsigned long long)bits);
        s->add(tmp);
    }
}

static void handle_decimals(const LookupRule* r, uint64_t bits, SBuf* s) {
    uint64_t mask = rule_mask(r);
    char tmp[96];
    if (mask == 0) return;
    int number = (int)(bits % mask);
    if (number == 0) {
        s->add(r->default_message ? r->default_message : "");
        s->add(" ");
    }
    for (int i = 0; i < r->num_maps; ++i) {
        const LookupMap* m = &r->maps[i];
        if ((m->value - (m->value % mask)) != 0) {
            snprintf(tmp, sizeof(tmp), "BAD_RULE_%s(from=%d modulomask=%d) ", r->name, (int)m->value, (int)r->mask);
            s->add(tmp);
        }
        int num = (int)(m->value % mask);
        if ((number - num) >= 0) {
            s->add(m->name);
            s->add(" ");
            number -= num;
        }
    }
    if (number > 0) {
        snprintf(tmp, sizeof(tmp), "%s_%d ", r->name, number);
        s->add(tmp);
    }
}

void lookup_translate(const LookupRule* rules, int num_rules, uint64_t bits, char* out, size_t out_max) {
    char total[288] = "";
    for (int i = 0; i < num_rules; ++i) {
        char one[288];
        SBuf s{ one, sizeof(one), 0 };
        one[0] = 0;
        switch (rules[i].type) {
        case MapType::BitToString: handle_bits(&rules[i], bits, &s); break;
        case MapType::IndexToString: handle_index(&rules[i], bits, &s); break;
        case MapType::DecimalsToString: handle_decimals(&rules[i], bits, &s); break;
        }
        char joined[288];
        status_join_empty(total, one, joined, sizeof(joined));
        snprintf(total, sizeof(total), "%s", joined);
    }
    trim_right(total);
    status_sort(total);
    snprintf(out, out_max, "%s", total);
}

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

void status_sort(char* s) {
    char buf[320];
    snprintf(buf, sizeof(buf), "%s", s);
    const char* toks[48];
    int n = 0;
    char* save = nullptr;
    for (char* t = strtok_r(buf, " ", &save); t && n < 48; t = strtok_r(nullptr, " ", &save)) toks[n++] = t;
    qsort(toks, (size_t)n, sizeof(toks[0]), cmp_str);
    size_t o = 0;
    size_t cap = strlen(s) + 1;
    s[0] = 0;
    const char* prev = nullptr;
    for (int i = 0; i < n; ++i) {
        if (prev && strcmp(prev, toks[i]) == 0) continue;
        size_t l = strlen(toks[i]);
        if (o + l + (o ? 1 : 0) + 1 > cap) break;
        if (o) s[o++] = ' ';
        memcpy(s + o, toks[i], l);
        o += l;
        s[o] = 0;
        prev = toks[i];
    }
    for (char* p = s; *p; ++p) if (*p == '~') *p = ' ';
}

static void copy_trim(const char* in, char* out, size_t out_max) {
    snprintf(out, out_max, "%s", in ? in : "");
    trim_right(out);
}

static bool is_empty_or_null(const char* s) { return s[0] == 0 || strcmp(s, "null") == 0; }

void status_join_ok(const char* aa, const char* bb, char* out, size_t out_max) {
    char a[288], b[288];
    copy_trim(aa, a, sizeof(a));
    copy_trim(bb, b, sizeof(b));
    if (a[0] == 0 || strcmp(a, "OK") == 0 || strcmp(a, "null") == 0) {
        snprintf(out, out_max, "%s", is_empty_or_null(b) ? "OK" : b);
        return;
    }
    if (b[0] == 0 || strcmp(b, "OK") == 0 || strcmp(b, "null") == 0) {
        snprintf(out, out_max, "%s", is_empty_or_null(a) ? "OK" : a);
        return;
    }
    snprintf(out, out_max, "%s %s", a, b);
}

void status_join_empty(const char* aa, const char* bb, char* out, size_t out_max) {
    char a[288], b[288];
    copy_trim(aa, a, sizeof(a));
    copy_trim(bb, b, sizeof(b));
    if (is_empty_or_null(a)) {
        snprintf(out, out_max, "%s", is_empty_or_null(b) ? "" : b);
        return;
    }
    if (is_empty_or_null(b)) {
        snprintf(out, out_max, "%s", a);
        return;
    }
    bool aok = strcmp(a, "OK") == 0, bok = strcmp(b, "OK") == 0;
    if (!aok && bok) { snprintf(out, out_max, "%s", a); return; }
    if (aok && !bok) { snprintf(out, out_max, "%s", b); return; }
    if (aok && bok) { snprintf(out, out_max, "%s", a); return; }
    snprintf(out, out_max, "%s %s", a, b);
}

void tpl_status_standard(uint8_t sts, char* out, size_t out_max) {
    if (sts == 0) { snprintf(out, out_max, "OK"); return; }
    char s[96] = "";
    if ((sts & 0x03) == 0x01) strcat(s, "BUSY ");
    if ((sts & 0x03) == 0x02) strcat(s, "ERROR ");
    if ((sts & 0x03) == 0x03) strcat(s, "ALARM ");
    if (sts & 0x04) strcat(s, "POWER_LOW ");
    if (sts & 0x08) strcat(s, "PERMANENT_ERROR ");
    if (sts & 0x10) strcat(s, "TEMPORARY_ERROR ");
    trim_right(s);
    snprintf(out, out_max, "%s", s);
}

} // namespace wmb
