// wM-Buster ADV — ixml runtime (backtracking matcher over compiled grammars).
// GPL-3.0
#include "wmbus/ixml.h"

#include <string.h>

namespace wmb {

namespace {

enum ContKind : uint8_t { K_SEQ, K_REP, K_CAPEND };

struct Cont {
    uint8_t kind;
    uint16_t node;
    uint16_t idx;
    uint16_t pos0;
    const Cont* next;
};

enum EvType : uint8_t { EV_START, EV_END, EV_HIDDEN };

struct Ev {
    uint8_t type;
    uint16_t pos;
    uint16_t a;   // rule for EV_START, length for EV_HIDDEN
};

#define IX_MAX_EVENTS 320
#define IX_MAX_DEPTH 600
#define IX_UNBOUNDED 0xFFFF

struct Matcher {
    const IxmlGrammar* g;
    const char* in;
    size_t len;
    Ev ev[IX_MAX_EVENTS];
    int nev;
    int depth;
    bool overflow;

    void push(uint8_t type, size_t pos, uint16_t a) {
        if (nev >= IX_MAX_EVENTS) { overflow = true; return; }
        ev[nev].type = type;
        ev[nev].pos = (uint16_t)pos;
        ev[nev].a = a;
        nev++;
    }

    bool class_ok(const IxmlNode& n, char ch) const {
        const char* r = g->literals + n.a;
        for (int i = 0; i + 1 < n.b; i += 2) {
            if (ch >= r[i] && ch <= r[i + 1]) return true;
        }
        return false;
    }

    // Simple nodes match a fixed number of chars without side effects
    // (except hidden literal spans).
    bool simple(const IxmlNode& n) const { return n.type == IX_LIT || n.type == IX_CLASS; }
    size_t simple_len(const IxmlNode& n) const { return n.type == IX_LIT ? n.b : (n.c ? n.c : 1); }
    bool simple_match(const IxmlNode& n, size_t pos) const {
        size_t l = simple_len(n);
        if (pos + l > len) return false;
        if (n.type == IX_LIT) return memcmp(in + pos, g->literals + n.a, l) == 0;
        for (size_t i = 0; i < l; ++i) if (!class_ok(n, in[pos + i])) return false;
        return true;
    }

    bool match(uint16_t node, size_t pos, const Cont* k);
    bool rep(uint16_t node, uint16_t count, size_t pos, const Cont* k);
    bool cont(const Cont* k, size_t pos);
};

bool Matcher::cont(const Cont* k, size_t pos) {
    if (!k) return pos == len;
    switch (k->kind) {
    case K_SEQ: {
        const IxmlNode& seq = g->nodes[k->node];
        uint16_t child = g->children[seq.a + k->idx];
        if (k->idx + 1 < seq.b) {
            Cont c{ K_SEQ, k->node, (uint16_t)(k->idx + 1), 0, k->next };
            return match(child, pos, &c);
        }
        return match(child, pos, k->next);
    }
    case K_REP:
        if (pos == k->pos0) return false; // empty iteration, stop looping
        return rep(k->node, k->idx, pos, k->next);
    case K_CAPEND: {
        int mark = nev;
        push(EV_END, pos, 0);
        if (cont(k->next, pos)) return true;
        nev = mark;
        return false;
    }
    }
    return false;
}

bool Matcher::rep(uint16_t node, uint16_t count, size_t pos, const Cont* k) {
    const IxmlNode& n = g->nodes[node];
    uint16_t child_i = g->children[n.a];
    const IxmlNode& child = g->nodes[child_i];
    uint16_t min = n.b, max = n.c;
    if (simple(child)) {
        size_t step = simple_len(child);
        int avail = 0;
        size_t p = pos;
        while (step > 0 && (max == IX_UNBOUNDED || count + avail < max) && simple_match(child, p)) {
            avail++;
            p += step;
        }
        for (int i = avail; i >= 0; --i) {
            if (count + i < min) break;
            int mark = nev;
            if (child.type == IX_LIT && (child.flags & IXF_HIDDEN)) {
                for (int j = 0; j < i; ++j) push(EV_HIDDEN, pos + j * step, (uint16_t)step);
            }
            if (cont(k, pos + i * step)) return true;
            nev = mark;
        }
        return false;
    }
    if (max == IX_UNBOUNDED || count < max) {
        Cont c{ K_REP, node, (uint16_t)(count + 1), (uint16_t)pos, k };
        int mark = nev;
        if (match(child_i, pos, &c)) return true;
        nev = mark;
    }
    if (count >= min) return cont(k, pos);
    return false;
}

bool Matcher::match(uint16_t node, size_t pos, const Cont* k) {
    if (++depth > IX_MAX_DEPTH || overflow) { depth--; return false; }
    const IxmlNode& n = g->nodes[node];
    int mark = nev;
    bool r = false;
    switch (n.type) {
    case IX_LIT:
    case IX_CLASS:
        if (simple_match(n, pos)) {
            if (n.type == IX_LIT && (n.flags & IXF_HIDDEN)) push(EV_HIDDEN, pos, n.b);
            r = cont(k, pos + simple_len(n));
        }
        break;
    case IX_EMPTY:
        r = cont(k, pos);
        break;
    case IX_SEQ:
        if (n.b == 0) {
            r = cont(k, pos);
        } else if (n.b == 1) {
            r = match(g->children[n.a], pos, k);
        } else {
            Cont c{ K_SEQ, node, 1, 0, k };
            r = match(g->children[n.a], pos, &c);
        }
        break;
    case IX_ALT:
        for (uint16_t i = 0; i < n.b && !r; ++i) {
            nev = mark;
            r = match(g->children[n.a + i], pos, k);
        }
        break;
    case IX_REP:
        r = rep(node, 0, pos, k);
        break;
    case IX_REF: {
        const IxmlRule& rule = g->rules[n.a];
        if (rule.dvk) {
            push(EV_START, pos, n.a);
            Cont c{ K_CAPEND, 0, 0, 0, k };
            r = match(rule.body, pos, &c);
        } else {
            r = match(rule.body, pos, k);
        }
        break;
    }
    default:
        break;
    }
    if (!r) nev = mark;
    depth--;
    return r;
}

} // namespace

bool ixml_match(const IxmlGrammar* g, const char* input, size_t len, IxmlResult* out) {
    static Matcher m;   // large event buffer: keep off the stack
    m.g = g;
    m.in = input;
    m.len = len;
    m.nev = 0;
    m.depth = 0;
    m.overflow = false;
    out->n = 0;
    const IxmlRule& start = g->rules[g->start_rule];
    bool ok;
    if (start.dvk) {
        m.push(EV_START, 0, g->start_rule);
        Cont c{ K_CAPEND, 0, 0, 0, nullptr };
        ok = m.match(start.body, 0, &c);
    } else {
        ok = m.match(start.body, 0, nullptr);
    }
    if (!ok) return false;

    // Build captures from the event log.
    int stack[16];
    int sp = 0;
    for (int i = 0; i < m.nev; ++i) {
        const Ev& e = m.ev[i];
        if (e.type == EV_START) {
            if (out->n >= WMB_IXML_MAX_CAPTURES || sp >= 16) continue;
            IxmlCapture* c = &out->caps[out->n];
            c->dvk = g->rules[e.a].dvk;
            c->start = e.pos;
            c->len = 0;
            c->text[0] = 0;
            stack[sp++] = out->n++;
        } else if (e.type == EV_END) {
            if (sp == 0) continue;
            IxmlCapture* c = &out->caps[stack[--sp]];
            size_t o = 0;
            for (size_t p = c->start; p < e.pos && o + 1 < sizeof(c->text); ++p) {
                bool hidden = false;
                for (int j = 0; j < m.nev; ++j) {
                    const Ev& h = m.ev[j];
                    if (h.type == EV_HIDDEN && p >= h.pos && p < (size_t)h.pos + h.a) { hidden = true; break; }
                }
                if (!hidden) c->text[o++] = input[p];
            }
            c->text[o] = 0;
            c->len = (uint16_t)o;
        }
    }
    return true;
}

} // namespace wmb
