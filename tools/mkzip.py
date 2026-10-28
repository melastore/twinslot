#!/usr/bin/env python3
"""Build TwinSlot recovery zips.

    tools/mkzip.py                       the standard set into out/
    tools/mkzip.py OUT.zip ARG [ARG...]  one zip that runs `twinslot ARG...`;
                                         `+` starts the next command
    --sign KEY (first option)            sign like signapk -w, so a stock
                                         recovery accepts it: KEY.x509.pem +
                                         KEY.pk8 (the ROM's releasekey)

Standard set: twinslot-switch-{other,a,b}.zip, twinslot-rebind-other.zip,
twinslot-setup.zip (half of userdata for the other slot) and
twinslot-remove.zip. Zips are reproducible: fixed timestamps and order.
"""
import os
import io
import shlex
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out")
STAMP = (2026, 1, 1, 0, 0, 0)

STANDARD = {
    "twinslot-switch-other.zip": ["switch", "other"],
    "twinslot-switch-a.zip": ["switch", "a"],
    "twinslot-switch-b.zip": ["switch", "b"],
    "twinslot-rebind-other.zip": ["rebind", "other"],
    "twinslot-setup.zip": ["setup", "--apply"],
    "twinslot-remove.zip": ["remove", "--apply"],
}


def add(zf, name, data, mode):
    info = zipfile.ZipInfo(name, STAMP)
    info.external_attr = (0o100000 | mode) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    zf.writestr(info, data)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def commands(args):
    """ARG... with `+` between commands -> the `command` file, one per line."""
    out, cur = [], []
    for a in args + ["+"]:
        if a == "+":
            if cur:
                out.append(" ".join(shlex.quote(x) for x in cur))
            cur = []
        else:
            cur.append(a)
    return ("\n".join(out) + "\n").encode()


def sign(data, key):
    """Whole-file signature as recovery checks it: a detached PKCS#7 over
    everything but the zip comment and its length, kept in the comment."""
    with tempfile.TemporaryDirectory() as tmp:
        body, pem = os.path.join(tmp, "body"), os.path.join(tmp, "key.pem")
        with open(body, "wb") as f:
            f.write(data[:-2])
        subprocess.run(["openssl", "pkcs8", "-inform", "DER", "-nocrypt", "-in", key + ".pk8", "-out", pem],
                       check=True)
        sig = subprocess.run(["openssl", "cms", "-sign", "-binary", "-noattr", "-nosmimecap", "-md", "sha256",
                              "-outform", "DER", "-signer", key + ".x509.pem", "-inkey", pem, "-in", body],
                             check=True, capture_output=True).stdout
    total = len(sig) + 6
    comment = sig + struct.pack("<HHH", total, 0xFFFF, total)
    if b"PK\x05\x06" in comment:
        sys.exit("signature contains an end-of-zip marker; sign again")
    return data[:-2] + struct.pack("<H", len(comment)) + comment


def build(path, args, tool, script, key=None, extra=None):
    """extra: {name: bytes} added after the command (table.bin, as the app adds it)."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        add(zf, "META-INF/com/google/android/update-binary", script, 0o755)
        add(zf, "META-INF/com/google/android/updater-script", b"#twinslot\n", 0o644)
        add(zf, "twinslot", tool, 0o755)
        add(zf, "command", commands(args), 0o644)
        for name, data in (extra or {}).items():
            add(zf, name, data, 0o644)
    data = buf.getvalue()
    with open(path, "wb") as f:
        f.write(sign(data, key) if key else data)
    print(path)


def main():
    binary = os.path.join(OUT, "twinslot")
    if not os.path.exists(binary):
        sys.exit("build the phone binary first: make device")
    tool = read(binary)
    script = read(os.path.join(ROOT, "recovery", "update-binary"))
    argv, key = sys.argv[1:], None
    if argv[:1] == ["--sign"] and len(argv) > 1:
        key, argv = argv[1], argv[2:]
    if len(argv) > 1:
        build(argv[0], argv[1:], tool, script, key)
    elif not argv:
        for name, args in STANDARD.items():
            build(os.path.join(OUT, name), args, tool, script, key)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
