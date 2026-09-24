// wM-Buster ADV — host regression test: decodes every upstream wmbusmeters
// driver test telegram and compares the result with the expected json.
// GPL-3.0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "wmbus/engine.h"
#include "wmbus/frame.h"
#include "json_mini.h"
#include "../vectors/driver_tests.h"

using namespace wmb;

static bool skip_key(const std::string& k) {
    return k == "_" || k == "media" || k == "meter" || k == "driver" || k == "name" || k == "id" ||
           k == "timestamp" || k == "device" || k == "rssi_dbm" || k == "fields";
}

static Decoder g_dec;

// A meter keeps the values of earlier telegrams (upstream prints all fields
// that ever got a value), so multi-telegram vectors check the merged state.
struct Seen {
    bool is_text;
    std::string text;
    double value;
    Unit unit;
    OutField copy;  // for field_format
};

static void merge_fields(const DecodeResult& r, std::map<std::string, Seen>* state) {
    for (int i = 0; i < r.num_fields; ++i) {
        const OutField& f = r.fields[i];
        if (f.hidden) continue;
        Seen s;
        s.is_text = f.is_text;
        s.text = f.is_text && f.text ? f.text : "";
        s.value = f.value;
        s.unit = f.unit;
        s.copy = f;
        s.copy.text = nullptr;
        (*state)[f.name] = s;
    }
}

int main(int argc, char** argv) {
    bool verbose = false;
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-v")) verbose = true;
        else only = argv[i];
    }
    int vec_ok = 0, vec_total = 0, fields_ok = 0, fields_total = 0, auto_ok = 0, legacy = 0;
    std::map<std::string, std::pair<int, int>> per_driver;

    for (unsigned vi = 0; vi < DRIVER_TEST_VECTORS_LEN; ++vi) {
        const DriverTestVector& v = DRIVER_TEST_VECTORS[vi];
        if (only && strcmp(only, v.driver) != 0) continue;
        std::vector<JsonKV> exp;
        if (!json_parse_flat(v.expected, &exp)) continue;

        uint8_t key[16];
        size_t key_len = 0;
        if (strcmp(v.key, "NOKEY") != 0 && (strlen(v.key) == 32 || strlen(v.key) == 16)) {
            key_len = hex_to_bytes(v.key, key, sizeof(key));
        }
        bool has_key = key_len == 16 || key_len == 8;
        const DriverDef* drv = driver_by_name(v.driver);
        if (!drv) {
            // Vectors for upstream's legacy hand written C++ drivers (qwater,
            // izar, ...). They are never auto detected upstream; their xmq
            // successors (qwaterv2, izarv2, ...) are ported and tested.
            legacy++;
            continue;
        }

        // Telegrams separated by ',' are fed in order (compact frames need
        // the full frame first); the last one is checked.
        std::string all = v.telegram;
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t c = all.find(',', start);
            parts.push_back(all.substr(start, c == std::string::npos ? std::string::npos : c - start));
            if (c == std::string::npos) break;
            start = c + 1;
        }
        compact_cache_clear();
        std::map<std::string, Seen> state;
        bool decoded = false;
        bool frame_ok = true;
        for (auto& p : parts) {
            std::string h;
            for (char ch : p) if (ch != '#') h.push_back(ch);
            uint8_t bytes[400];
            size_t n = hex_to_bytes(h.c_str(), bytes, sizeof(bytes));
            Frame f;
            if (!n || !frame_from_bytes(bytes, n, &f)) { frame_ok = false; continue; }
            DecodeOptions o{};
            o.key = has_key ? key : nullptr;
            o.key_len = (uint8_t)key_len;
            o.simulated = true;
            o.decode_bad_tag = true;  // compare with upstream, which decodes it
            o.forced_driver = drv;
            o.generic_fallback = true;
            decoded = wmbus_decode(&g_dec, f, o);
            if (&p == &parts.back()) {
                // auto detection check on the final telegram
                DecodeOptions oa = o;
                oa.forced_driver = nullptr;
                static Decoder da;
                wmbus_decode(&da, f, oa);
                if (da.res.driver && drv && da.res.driver == drv) auto_ok++;
                else if (verbose) printf("AUTO %s/%s detected %s\n", v.driver, v.name, da.res.driver ? da.res.driver->name : "-");
                decoded = wmbus_decode(&g_dec, f, o);
            }
            merge_fields(g_dec.res, &state);
        }
        vec_total++;
        per_driver[v.driver].second++;
        const DecodeResult& r = g_dec.res;
        int fo = 0, ft = 0;
        std::string fails;
        for (auto& kv : exp) {
            if (skip_key(kv.key)) continue;
            ft++;
            const OutField* of = nullptr;
            auto it = state.find(kv.key);
            if (it != state.end()) {
                it->second.copy.text = it->second.text.c_str();
                of = &it->second.copy;
            }
            bool m = false;
            char got[128] = "<missing>";
            if (of) {
                if (of->is_text) snprintf(got, sizeof(got), "\"%s\"", of->text);
                else if (unit_quantity(of->unit) == Quantity::PointInTime && of->unit != Unit::UnixTimestamp) {
                    char b[64];
                    if (std::isnan(of->value)) snprintf(b, sizeof(b), "null");
                    else field_format(of, b, sizeof(b), false);
                    snprintf(got, sizeof(got), "\"%s\"", b);
                } else snprintf(got, sizeof(got), "%.9g", of->value);
            }
            if (of) {
                if (kv.is_null) {
                    m = of->is_text ? (!strcmp(of->text, "null")) : std::isnan(of->value);
                } else if (kv.is_str) {
                    if (of->is_text) m = kv.str == of->text;
                    else if (unit_quantity(of->unit) == Quantity::PointInTime) {
                        char b[64];
                        field_format(of, b, sizeof(b), false);
                        m = !std::isnan(of->value) && kv.str == b;
                    }
                } else if (!of->is_text) {
                    double d = std::fabs(of->value - kv.num);
                    m = !std::isnan(of->value) && d <= 0.0005 + 1e-6 * std::fabs(kv.num);
                }
            }
            if (m) fo++;
            else {
                char line[512];
                snprintf(line, sizeof(line), "    %s: expected %s got %s\n", kv.key.c_str(),
                         kv.is_null ? "null" : kv.is_str ? ("\"" + kv.str + "\"").c_str() : std::to_string(kv.num).c_str(), got);
                fails += line;
            }
        }
        fields_ok += fo;
        fields_total += ft;
        bool pass = frame_ok && fo == ft;
        if (pass) { vec_ok++; per_driver[v.driver].first++; }
        if (!pass && verbose) {
            printf("FAIL %s/%s (%s%s, %s)\n%s", v.driver, v.name, decode_status_name(r.status),
                   decoded ? "" : " no-decode", decrypt_status_name(r.decrypt), fails.c_str());
        }
    }
    int drivers_full = 0;
    for (auto& p : per_driver) if (p.second.first == p.second.second) drivers_full++;
    printf("vectors: %d/%d fully correct, fields %d/%d, auto-detect %d/%d, drivers fully passing %d/%zu"
           " (%d legacy C++ driver vectors skipped)\n",
           vec_ok, vec_total, fields_ok, fields_total, auto_ok, vec_total, drivers_full, per_driver.size(), legacy);
    if (verbose) {
        for (auto& p : per_driver) {
            if (p.second.first != p.second.second) printf("  %-16s %d/%d\n", p.first.c_str(), p.second.first, p.second.second);
        }
    }
    return vec_ok == vec_total ? 0 : 1;
}
