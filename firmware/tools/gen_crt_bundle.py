#!/usr/bin/env python3
"""Erzeugt data/x509_crt_bundle.bin (ESP-IDF-Format) aus Mozillas cacert.pem.

  curl -o cacert.pem https://curl.se/ca/cacert.pem
  python tools/gen_crt_bundle.py cacert.pem data/x509_crt_bundle.bin

Format: u16 Anzahl, je Zertifikat u16 Namenslänge, u16 Schlüssellänge,
Subject (DER), Public Key (DER) – nach Subject sortiert (Binärsuche).
"""
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import serialization


def main(src, dst):
    certs = x509.load_pem_x509_certificates(open(src, "rb").read())
    certs = sorted(certs, key=lambda c: c.subject.public_bytes())
    out = struct.pack(">H", len(certs))
    for c in certs:
        name = c.subject.public_bytes()
        key = c.public_key().public_bytes(serialization.Encoding.DER,
                                          serialization.PublicFormat.SubjectPublicKeyInfo)
        out += struct.pack(">HH", len(name), len(key)) + name + key
    open(dst, "wb").write(out)
    print(f"{len(certs)} Zertifikate, {len(out)} Bytes -> {dst}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
