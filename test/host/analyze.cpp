// wM-Buster ADV — host analyzer: decode a telegram given as hex.
//   build/analyze <hex> [driver|auto] [key]
// GPL-3.0
#include <cmath>
#include <cstdio>
#include <cstring>

#include "wmbus/engine.h"
#include "wmbus/frame.h"

using namespace wmb;

static Decoder dec;

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <hex> [driver|auto] [key]\n", argv[0]);
        return 2;
    }
    uint8_t bytes[512];
    size_t n = hex_to_bytes(argv[1], bytes, sizeof(bytes));
    Frame f;
    if (!n || !frame_from_bytes(bytes, n, &f)) {
        printf("not a frame\n");
        return 1;
    }
    DecodeOptions o{};
    o.generic_fallback = true;
    if (argc > 2 && strcmp(argv[2], "auto") != 0) o.forced_driver = driver_by_name(argv[2]);
    uint8_t key[16];
    if (argc > 3 && hex_to_bytes(argv[3], key, 16) == 16) o.key = key;
    bool ok = wmbus_decode(&dec, f, o);
    const Telegram& t = dec.t;
    const DecodeResult& r = dec.res;
    printf("frame: %u bytes format=%d mode=%s\n", f.len, (int)f.format, link_mode_name(f.mode));
    printf("dll: c=%02X mfct=%s id=%s ver=%02X type=%02X ci=%02X\n", t.dll_c, r.mfct, t.dll_id, t.dll_version,
           t.dll_type, t.tpl_ci);
    printf("tpl: long=%d acc=%02X sts=%02X cfg=%04X sec=%d blocks=%d header=%u compact=%d sig=%04X\n", t.tpl_id_found,
           t.tpl_acc, t.tpl_sts, t.tpl_cfg, t.tpl_sec_mode, t.tpl_num_encr_blocks, t.header_size, t.compact,
           t.format_signature);
    printf("decrypt: %s  status: %s  driver: %s  ok=%d\n", decrypt_status_name(r.decrypt),
           decode_status_name(r.status), r.driver ? r.driver->name : "-", ok);
    char hex[600];
    bytes_to_hex(t.payload(), t.payload_len(), hex, sizeof(hex));
    printf("payload: %s\n", hex);
    for (int i = 0; i < dec.dv.n; ++i) {
        const DvEntry& e = dec.dv.e[i];
        char v[128];
        dv_extract_hex(&e, v, sizeof(v));
        printf("  dv %3u %-16s vif=%04X mt=%d st=%u ta=%u su=%u nc=%d%s %s\n", e.offset, e.key, e.vif, (int)e.mtype,
               (unsigned)e.storage, e.tariff, e.subunit, e.ncomb, e.synthetic ? " syn" : "", v);
    }
    for (int i = 0; i < r.num_fields; ++i) {
        const OutField& fl = r.fields[i];
        char val[160];
        field_format(&fl, val, sizeof(val));
        printf("  %s%-40s %s\n", fl.hidden ? "(h) " : "", fl.name, val);
    }
    static char json[4096];
    result_to_json(&r, json, sizeof(json), "x", nullptr);
    printf("%s\n", json);
    return 0;
}
