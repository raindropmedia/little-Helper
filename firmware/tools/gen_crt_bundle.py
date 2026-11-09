#!/usr/bin/env python3
"""Erzeugt data/x509_crt_bundle.bin (ESP-IDF-Format) aus Mozillas cacert.pem
plus den Zusatz-Roots in tools/extra_roots/*.pem.

  curl -o cacert.pem https://curl.se/ca/cacert.pem
  python tools/gen_crt_bundle.py cacert.pem data/x509_crt_bundle.bin

Zusatz-Roots: Mozilla hat einige Roots entfernt, die große Dienste in ihren
Ketten noch als Querzertifikat mitsenden (z. B. GlobalSign Root CA bei Google
Trust Services -> api.anthropic.com). Die ESP32-Prüfung (Arduino 2.x) prüft nur
das oberste mitgesendete Zertifikat, daher müssen diese Roots enthalten sein.

Format: u16 Anzahl, je Zertifikat u16 Namenslänge, u16 Schlüssellänge,
Subject (DER), Public Key (DER) – nach Subject sortiert (Binärsuche).
"""
import glob
import os
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import serialization

HERE = os.path.dirname(os.path.abspath(__file__))


def main(src, dst):
    certs = x509.load_pem_x509_certificates(open(src, "rb").read())
    for extra in sorted(glob.glob(os.path.join(HERE, "extra_roots", "*.pem"))):
        certs += x509.load_pem_x509_certificates(open(extra, "rb").read())
    entries = {}
    for c in certs:
        name = c.subject.public_bytes()
        key = c.public_key().public_bytes(serialization.Encoding.DER,
                                          serialization.PublicFormat.SubjectPublicKeyInfo)
        entries[(name, key)] = None  # Duplikate entfernen
    items = sorted(entries, key=lambda e: e[0])
    out = struct.pack(">H", len(items))
    for name, key in items:
        out += struct.pack(">HH", len(name), len(key)) + name + key
    open(dst, "wb").write(out)
    print(f"{len(items)} Zertifikate, {len(out)} Bytes -> {dst}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
