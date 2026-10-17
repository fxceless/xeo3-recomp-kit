r"""CON-sign a content package header with YOUR emulator's own console key, the way the
emulator signs every package it creates (profiles, saves, caches).

    python -I tools\consign.py sign   <package> --emu "<console folder>\Emu.exe" [--xex <game default.xex>]
    python -I tools\consign.py verify <package> --emu "<console folder>\Emu.exe"

WHY. The dashboard's My Games lists what xam lists, and xam (the retail one) verifies every
package header it enumerates: a LIVE/PIRS header against Microsoft's public keys, a CON
header against the console certificate it carries. A Games-on-Demand package made with
iso2god is a LIVE header with a blank signature, so xam reads it, rejects it, and the dash
says "no games found". The emulator ships a console key vault (certificate + private key)
and signs its own packages as CON with it; this signs yours the same way, so xam accepts it
exactly as it accepts a profile the emulator created.

NOTHING OF THE KEY IS IN THIS FILE. It is located in the Emu.exe you pass, by structure: an
XECRYPT_RSAPRV_1024 block (qword count 0x10, exponent 0x10001) whose p*q equals its modulus,
and the 0x1A8-byte console certificate that carries the same modulus. Nothing is written
anywhere but the package.

`sign` also sets the header metadata the way this console would have written it (platform =
Xbox 360, console id = the certificate's, version/base version = the game XEX's when --xex
is given), recomputes the header hash and writes magic, certificate and signature. The
unsigned original is kept once as <package>.unsigned.
"""
import argparse, hashlib, os, struct, sys

DIGESTINFO = bytes.fromhex("3021300906052b0e03021a05000414")   # PKCS#1 v1.5, SHA-1


def qw_swap(b):
    """XeCrypt BnQw layout <-> big-endian integer bytes: qwords keep their byte order, the
    order of the qwords is reversed."""
    return b"".join(b[i:i + 8] for i in range(len(b) - 8, -1, -8))


def load_key(emu):
    d = open(emu, "rb").read()
    pos = 0
    while True:
        i = d.find(b"\x00\x00\x00\x10\x00\x01\x00\x01", pos)   # cqw = 0x10, e = 0x10001
        if i < 0:
            break
        pos = i + 4
        k = d[i:i + 0x1D0]
        if len(k) < 0x110:
            break
        for f in (lambda b: b, qw_swap):
            n = int.from_bytes(f(k[0x10:0x90]), "big")
            p = int.from_bytes(f(k[0x90:0xD0]), "big")
            q = int.from_bytes(f(k[0xD0:0x110]), "big")
            if n.bit_length() < 1000 or p * q != n:
                continue
            # the certificate stores the modulus the same way, 0x28 bytes in
            j = d.find(k[0x10:0x90])
            while j >= 0:
                c = j - 0x28
                if c >= 0 and d[c:c + 2] == b"\x01\xa8":
                    return d[c:c + 0x1A8], n, 0x10001, p, q
                j = d.find(k[0x10:0x90], j + 1)
    sys.exit(f"{emu}: no console key vault found (is this the emulator's Emu.exe?)")


def pkcs1(digest, k=128):
    t = DIGESTINFO + digest
    return b"\x00\x01" + b"\xff" * (k - 3 - len(t)) + b"\x00" + t


def header_hash(d):
    hs = struct.unpack_from(">I", d, 0x340)[0]
    return hashlib.sha1(d[0x344:(hs + 0xFFF) & ~0xFFF]).digest()


def xex_versions(xex):
    d = open(xex, "rb").read(0x20000)
    if d[:4] != b"XEX2":
        sys.exit(f"{xex} is not an Xbox 360 executable")
    for i in range(struct.unpack_from(">I", d, 0x14)[0]):
        key, val = struct.unpack_from(">II", d, 0x18 + 8 * i)
        if key == 0x00040006:                                   # execution info
            _media, ver, base, tid = struct.unpack_from(">IIII", d, val)
            return ver, base, tid
    sys.exit(f"{xex}: no execution-info header")


def verify(pkg, emu):
    d = open(pkg, "rb").read()
    cert, n, e, _, _ = load_key(emu)
    s = d[0x1AC:0x22C][::-1]                       # stored byte-reversed
    m = pow(int.from_bytes(s, "big"), e, n).to_bytes(128, "big")
    sig_ok = m == pkcs1(hashlib.sha1(d[0x22C:0x344]).digest())
    hh_ok = header_hash(d) == d[0x32C:0x340]
    print(f"{pkg}: magic {d[:4]!r}  this console's certificate: {d[4:0x1AC] == cert}  "
          f"signature: {'OK' if sig_ok else 'BAD'}  header hash: {'OK' if hh_ok else 'BAD'}")
    return d[:4] == b"CON " and sig_ok and hh_ok


def sign(pkg, emu, xex):
    d = bytearray(open(pkg, "rb").read())
    if len(d) < 0x1000 or d[:4] not in (b"CON ", b"LIVE", b"PIRS"):
        sys.exit(f"{pkg} is not an STFS/SVOD content package header")
    cert, n, e, p, q = load_key(emu)
    tid = struct.unpack_from(">I", d, 0x360)[0]
    if xex:
        ver, base, xtid = xex_versions(xex)
        if xtid != tid:
            sys.exit(f"the package is title {tid:08X} but {xex} is {xtid:08X} -- wrong game?")
        struct.pack_into(">II", d, 0x358, ver, base)
    d[0x364] = 2                                   # platform: Xbox 360
    d[0x36C:0x371] = cert[2:7]                     # console id = this console's
    d[0x32C:0x340] = header_hash(d)
    d[0:4] = b"CON "
    d[4:0x1AC] = cert
    m = int.from_bytes(pkcs1(hashlib.sha1(bytes(d[0x22C:0x344])).digest()), "big")
    sig = pow(m, pow(e, -1, (p - 1) * (q - 1)), n).to_bytes(128, "big")
    d[0x1AC:0x22C] = sig[::-1]                     # the emulator's own packages store it reversed
    bak = pkg + ".unsigned"
    if not os.path.exists(bak):
        os.replace(pkg, bak)
    open(pkg, "wb").write(d)
    print(f"signed {pkg} (title {tid:08X})")
    if not verify(pkg, emu):
        sys.exit("verification of the signed header FAILED")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["sign", "verify"])
    ap.add_argument("package")
    ap.add_argument("--emu", required=True, help="Emu.exe of the console folder the package is for")
    ap.add_argument("--xex", help="the game's default.xex (sets the header's version fields)")
    a = ap.parse_args()
    if a.cmd == "verify":
        sys.exit(0 if verify(a.package, a.emu) else 1)
    sign(a.package, a.emu, a.xex)


if __name__ == "__main__":
    main()
