#!/usr/bin/env python3
"""Writes the root CA bundle the firmware uses to verify HTTPS servers (ntfy).

    python3 tools/gen_ca_bundle.py cacert.pem [src/net/ca_bundle.dat]

cacert.pem is Mozilla's CA list, e.g. https://curl.se/ca/cacert.pem or the
file from Python's certifi package. The output is the ESP-IDF x509 certificate
bundle format read by WiFiClientSecure::setCACertBundle(): a big-endian
certificate count, then for each root, sorted by subject, the subject name
length, the public key length, the DER subject name and the DER public key.
No third-party modules are needed.
"""
import base64
import re
import sys


def tlv(buf, pos):
    """Returns (tag, header_len, value_len) of the DER element at pos."""
    tag = buf[pos]
    n = buf[pos + 1]
    if n < 0x80:
        return tag, 2, n
    k = n & 0x7F
    return tag, 2 + k, int.from_bytes(buf[pos + 2:pos + 2 + k], "big")


def children(buf, pos):
    """Yields (start, end) of each element inside the constructed element at pos."""
    _, h, n = tlv(buf, pos)
    p, end = pos + h, pos + h + n
    while p < end:
        _, ch, cn = tlv(buf, p)
        yield p, p + ch + cn
        p += ch + cn


def subject_and_key(der):
    tbs = next(children(der, 0))[0]
    fields = list(children(der, tbs))
    if der[fields[0][0]] == 0xA0:  # explicit version
        fields = fields[1:]
    # serialNumber, signature, issuer, validity, subject, subjectPublicKeyInfo
    subj, spki = fields[4], fields[5]
    return der[subj[0]:subj[1]], der[spki[0]:spki[1]]


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "src/net/ca_bundle.dat"
    pem = open(src, "r", encoding="ascii", errors="ignore").read()
    certs = {}
    for b64 in re.findall(r"-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----", pem, re.S):
        name, key = subject_and_key(base64.b64decode("".join(b64.split())))
        certs[name] = key  # same subject twice: the later one wins, like mbedTLS
    blob = bytearray(len(certs).to_bytes(2, "big"))
    for name in sorted(certs):
        key = certs[name]
        blob += len(name).to_bytes(2, "big") + len(key).to_bytes(2, "big") + name + key
    open(out, "wb").write(blob)
    print("%s: %d roots, %d bytes" % (out, len(certs), len(blob)))


if __name__ == "__main__":
    main()
