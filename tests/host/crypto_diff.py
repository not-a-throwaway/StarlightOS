#!/usr/bin/env python3
"""Differential test of crypto.c against hashlib / hmac / cryptography (OpenSSL).
usage: crypto_diff.py /path/to/crypto_cli"""
import sys, os, hmac, hashlib, random, subprocess
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes
from cryptography.exceptions import InvalidTag

random.seed(1234)
cli = sys.argv[1]
cases, expect = [], []
def add(line, exp): cases.append(line); expect.append(exp)
rb = lambda n: bytes(random.getrandbits(8) for _ in range(n))

# ---------- fixed, well-known vectors ----------
add("sha256 616263", ["ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"])
add("sha256", ["e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"])
add("sha384 616263", ["cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"])
add("aes 000102030405060708090a0b0c0d0e0f 00112233445566778899aabbccddeeff", ["69c4e0d86a7b0430d8cdb78070b4c55a"])
add("aes 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f 00112233445566778899aabbccddeeff", ["8ea2b7ca516745bfeafc49904b496089"])
add("x25519 77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", ["4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"])
add("x25519base 77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", ["8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"])
add("gcmenc 00000000000000000000000000000000 000000000000000000000000 - 00000000000000000000000000000000",
    ["0388dace60b6a392f328c2b971b2fe78", "ab6e47d42cec13bdf53a67b21257bddf"])
# RFC 8439 2.8.2 AEAD test vector
add("ccenc 808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f 070000004041424344454647 50515253c0c1c2c3c4c5c6c7 4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a204966204920636f756c64206f6666657220796f75206f6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73637265656e20776f756c642062652069742e",
    ["d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116", "1ae10b594f09e26a7e902ecbd0600691"])
# P-256: private key 1 gives the generator
add("p256pub " + "00"*31 + "01", ["046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"])

# ---------- random differential cases ----------
for n in list(range(0, 130)) + [255, 256, 1000, 4097]:
    m = rb(n); add("sha256 " + (m.hex() or "-"), [hashlib.sha256(m).hexdigest()])
    add("sha384 " + (m.hex() or "-"), [hashlib.sha384(m).hexdigest()])
for n in (0, 1, 3, 111, 112, 113, 127, 128, 129, 255, 1000):
    m = rb(n) if n else b""
    add("hash384split " + (m.hex() or "-"), [hashlib.sha384(m).hexdigest()])
for alg, hn in ((256, hashlib.sha256), (384, hashlib.sha384)):
    for _ in range(40):
        k, m = rb(random.choice([1, 16, 32, 48, 63, 64, 65, 100, 127, 128, 129, 200])), rb(random.randint(0, 300))
        add(f"hmac {alg} {k.hex()} {m.hex() or '-'}", [hmac.new(k, m, hn).hexdigest()])
    for _ in range(25):
        salt, ikm, info, L = rb(random.randint(1, 60)), rb(random.randint(1, 60)), rb(random.randint(0, 60)), random.choice([1, 12, 16, 32, 33, 48, 49, 64, 100])
        ref = HKDF(algorithm=hashes.SHA256() if alg == 256 else hashes.SHA384(), length=L, salt=salt, info=info or None).derive(ikm)
        add(f"hkdf {alg} {salt.hex()} {ikm.hex()} {info.hex() or '-'} {L}", [ref.hex()])

def prf_ref(alg, secret, label, seed, n):          # RFC 5246 section 5, independent Python implementation
    hn = hashlib.sha256 if alg == 256 else hashlib.sha384
    ls = label + seed; A = ls; out = b""
    while len(out) < n:
        A = hmac.new(secret, A, hn).digest(); out += hmac.new(secret, A + ls, hn).digest()
    return out[:n]
for alg in (256, 384):
    for L in (12, 40, 48, 104, 128, 136):
        sec, seed = rb(random.choice([32, 48])), rb(64)
        lab = random.choice([b"master secret", b"key expansion", b"client finished", b"extended master secret"])
        add(f"prf {alg} {sec.hex()} {lab.decode().replace(chr(32), chr(95))} {seed.hex()} {L}", [prf_ref(alg, sec, lab, seed, L).hex()])

for klen in (16, 32):
    for _ in range(40):
        k, b = rb(klen), rb(16)
        e = Cipher(algorithms.AES(k), modes.ECB()).encryptor(); add(f"aes {k.hex()} {b.hex()}", [(e.update(b) + e.finalize()).hex()])
    for _ in range(80):
        k, iv = rb(klen), rb(12); aad = rb(random.choice([0, 5, 13, 16, 17, 40])); pt = rb(random.choice([0, 1, 15, 16, 17, 31, 32, 100, 1000, 16384, 16385]))
        ct_tag = AESGCM(k).encrypt(iv, pt, aad or None); ct, tag = ct_tag[:-16], ct_tag[-16:]
        a, p = aad.hex() or "-", pt.hex() or "-"
        add(f"gcmenc {k.hex()} {iv.hex()} {a} {p}", [ct.hex(), tag.hex()])
        add(f"gcmdec {k.hex()} {iv.hex()} {a} {ct.hex() or '-'} {tag.hex()}", [pt.hex()])
        bad = bytearray(tag); bad[random.randrange(16)] ^= 1 << random.randrange(8)
        add(f"gcmdec {k.hex()} {iv.hex()} {a} {ct.hex() or '-'} {bytes(bad).hex()}", ["AUTHFAIL"])
for _ in range(100):
    k, nonce = rb(32), rb(12); aad = rb(random.choice([0, 5, 13, 16, 17, 40])); pt = rb(random.choice([0, 1, 15, 16, 17, 31, 32, 63, 64, 65, 100, 1000, 16384, 16385]))
    ct_tag = ChaCha20Poly1305(k).encrypt(nonce, pt, aad or None); ct, tag = ct_tag[:-16], ct_tag[-16:]
    a, p = aad.hex() or "-", pt.hex() or "-"
    add(f"ccenc {k.hex()} {nonce.hex()} {a} {p}", [ct.hex(), tag.hex()])
    add(f"ccdec {k.hex()} {nonce.hex()} {a} {ct.hex() or '-'} {tag.hex()}", [pt.hex()])
    bad = bytearray(tag); bad[random.randrange(16)] ^= 1 << random.randrange(8)
    add(f"ccdec {k.hex()} {nonce.hex()} {a} {ct.hex() or '-'} {bytes(bad).hex()}", ["AUTHFAIL"])
for _ in range(25):
    sk, pk = rb(32), rb(32)
    priv = X25519PrivateKey.from_private_bytes(sk)
    pub = priv.public_key().public_bytes_raw()
    add(f"x25519base {sk.hex()}", [pub.hex()])
    peer = X25519PrivateKey.generate().public_key()
    add(f"x25519 {sk.hex()} {peer.public_bytes_raw().hex()}", [priv.exchange(peer).hex()])

from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
def p256_pub_bytes(k): return ec.derive_private_key(k, ec.SECP256R1()).public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)
edge = [1, 2, 3, N - 1, N - 2, 0xFFFFFFFF, 1 << 255, (1 << 128) + 12345]
for _ in range(60): edge.append(random.randrange(1, N))
for k in edge:
    kb = k.to_bytes(32, "big")
    add(f"p256pub {kb.hex()}", [p256_pub_bytes(k).hex()])
    peer_priv = ec.generate_private_key(ec.SECP256R1()); peer_pub = peer_priv.public_key()
    shared = ec.derive_private_key(k, ec.SECP256R1()).exchange(ec.ECDH(), peer_pub)
    add(f"p256ecdh {kb.hex()} {peer_pub.public_bytes(Encoding.X962, PublicFormat.UncompressedPoint).hex()}", [shared.hex()])
# invalid inputs must be rejected: off-curve point, bad prefix, zero scalar, scalar == n
good = p256_pub_bytes(7)
add(f"p256ecdh {(5).to_bytes(32,'big').hex()} {(good[:-1] + bytes([good[-1] ^ 1])).hex()}", ["FAIL"])
add(f"p256ecdh {(5).to_bytes(32,'big').hex()} 05{good[1:].hex()}", ["FAIL"])
add(f"p256pub {'00'*32}", ["FAIL"]); add(f"p256pub {N.to_bytes(32,'big').hex()}", ["FAIL"])

add(f"p256pub {'00'*32}", ["FAIL"]); add(f"p256pub {N.to_bytes(32,'big').hex()}", ["FAIL"])

# ---------- M9.1: big-int modexp, RSA (v1.5 + PSS) and ECDSA verification ----------
from cryptography.hazmat.primitives.asymmetric import rsa, padding, utils

def _hb(x):  # minimal big-endian integer to hex (used for moduli/exponents)
    return x.to_bytes((x.bit_length() + 7) // 8 or 1, "big").hex()

for bits in (512, 1024, 2048, 3072, 4096):
    for _ in range(3):
        m = random.getrandbits(bits) | (1 << (bits - 1)) | 1
        bb = random.randrange(1, m)
        ee = random.choice([3, 65537, random.randrange(1, 1 << 60)])
        add(f"modexp {_hb(bb)} {_hb(ee)} {_hb(m)}", [pow(bb, ee, m).to_bytes(bits // 8, "big").hex()])

# RFC 8017 DigestInfo prefixes for SHA-256 / SHA-384.
DI = {256: "3031300d060960864801650304020105000420", 384: "3041300d060960864801650304020205000430"}
for bits in (2048, 3072, 4096):
    for _ in range(3):
        k = rsa.generate_private_key(public_exponent=65537, key_size=bits)
        pn = k.public_key().public_numbers()
        nh, eh = _hb(pn.n), _hb(pn.e)
        for hn in (256, 384):
            hf = hashes.SHA256() if hn == 256 else hashes.SHA384()
            digest = (hashlib.sha256 if hn == 256 else hashlib.sha384)(msg := rb(random.randrange(1, 200))).hexdigest()
            sig = k.sign(msg, padding.PKCS1v15(), hf)
            add(f"rsav15 {nh} {eh} {sig.hex()} {DI[hn]}{digest}", ["OK"])
            add(f"rsav15 {nh} {eh} {sig.hex()} {DI[hn]}{'00' * (hn // 8)}", ["FAIL"])          # wrong digest
            add(f"rsav15 {nh} {eh} {'00' * (bits // 8)} {DI[hn]}{digest}", ["FAIL"])             # bad signature
            psig = k.sign(msg, padding.PSS(mgf=padding.MGF1(hf), salt_length=padding.PSS.DIGEST_LENGTH), hf)
            add(f"rsapss {nh} {eh} {psig.hex()} {hn} {digest}", ["OK"])
            bad = bytearray(bytes.fromhex(digest)); bad[0] ^= 0xff
            add(f"rsapss {nh} {eh} {psig.hex()} {hn} {bytes(bad).hex()}", ["FAIL"])              # wrong digest
            b2 = bytearray(psig); b2[random.randrange(len(b2))] ^= 1 << random.randrange(8)
            add(f"rsapss {nh} {eh} {bytes(b2).hex()} {hn} {digest}", ["FAIL"])                   # tampered sig

for curve, tag, hn in ((ec.SECP256R1(), "p256", 256), (ec.SECP384R1(), "p384", 384)):
    fs = 32 if tag == "p256" else 48
    for _ in range(12):
        k = ec.generate_private_key(curve)
        pub = k.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)
        msg = rb(random.randrange(1, 200))
        hf = hashes.SHA256() if hn == 256 else hashes.SHA384()
        r, s = utils.decode_dss_signature(k.sign(msg, ec.ECDSA(hf)))
        raw = r.to_bytes(fs, "big") + s.to_bytes(fs, "big")
        digest = (hashlib.sha256 if hn == 256 else hashlib.sha384)(msg).hexdigest()
        add(f"ecdsaverify {tag} {pub.hex()} {raw.hex()} {digest}", ["OK"])
        bad = bytearray(raw); bad[random.randrange(len(bad))] ^= 1 << random.randrange(8)
        add(f"ecdsaverify {tag} {pub.hex()} {bytes(bad).hex()} {digest}", ["FAIL"])               # tampered sig
        wrong = bytearray(bytes.fromhex(digest)); wrong[0] ^= 0xff
        add(f"ecdsaverify {tag} {pub.hex()} {raw.hex()} {bytes(wrong).hex()}", ["FAIL"])          # wrong digest
        off = bytearray(pub); off[1 + random.randrange(2 * fs)] ^= 1 << random.randrange(8)
        add(f"ecdsaverify {tag} {bytes(off).hex()} {raw.hex()} {digest}", ["FAIL"])               # off-curve key

inp = "\n".join(cases) + "\n"
out = subprocess.run([cli], input=inp, capture_output=True, text=True).stdout.split("\n")
pos, fails = 0, 0
for line, exp in zip(cases, expect):
    got = out[pos:pos+len(exp)]; pos += len(exp)
    if got != exp:
        fails += 1; print("FAIL:", line[:90], "\n  got ", got[0][:80] if got else None, "\n  want", exp[0][:80])
print(f"{len(cases)-fails}/{len(cases)} cases passed")
sys.exit(1 if fails else 0)
