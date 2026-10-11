#!/usr/bin/env python3
"""Differential test for x509.c against the 'cryptography' (OpenSSL) library.

Builds real certificates (RSA / ECDSA P-256 / P-384, v1.5 / PSS / ECDSA
signatures, SANs, wildcards, validity windows), then checks that the
freestanding parser and path validator in x509.c agree with what OpenSSL
would say.  Usage:  x509_diff.py <path-to-x509-cli>
"""
import datetime, subprocess, sys, hashlib, random
from cryptography import x509
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec, padding

CLI = sys.argv[1] if len(sys.argv) > 1 else "./x509_cli"
D = datetime.datetime
UTC = datetime.timezone.utc
NOW = 1700000000            # 2023-11-14
UNSET = object()

random.seed(0x51A9)

def key(kind):
    return {"rsa":   lambda: rsa.generate_private_key(public_exponent=65537, key_size=2048),
            "rsa4k": lambda: rsa.generate_private_key(public_exponent=65537, key_size=4096),
            "p256":  lambda: ec.generate_private_key(ec.SECP256R1()),
            "p384":  lambda: ec.generate_private_key(ec.SECP384R1())}[kind]()

def is_rsa(k): return isinstance(k, rsa.RSAPrivateKey)

def make(cn, issuer_cn, pub, signer, ca, san=None, nb=D(2020, 1, 1), na=D(2035, 1, 1),
         alg=hashes.SHA256(), pss=False, serial=None):
    b = (x509.CertificateBuilder()
         .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]))
         .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, issuer_cn)]))
         .public_key(pub)
         .serial_number(serial if serial is not None else x509.random_serial_number())
         .not_valid_before(nb).not_valid_after(na)
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if ca:
        b = b.add_extension(x509.KeyUsage(True, False, False, False, False, True, True, False, False), critical=True)
    if san is not None:
        b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName(s) for s in san]), critical=False)
    if not ca:
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    kw = dict(rsa_padding=padding.PSS(mgf=padding.MGF1(alg), salt_length=alg.digest_size)) if pss else {}
    return b.sign(signer, alg, **kw)

def H(c):
    if isinstance(c, (bytes, bytearray)): return bytes(c).hex()
    return c.public_bytes(serialization.Encoding.DER).hex()

def ts(dt):
    if dt.tzinfo is None: dt = dt.replace(tzinfo=UTC)
    return int(dt.timestamp())

def cn_of(c):
    return c.subject.get_attributes_for_oid(NameOID.COMMON_NAME)[0].value

def has_ext(c, cls):
    try: c.extensions.get_extension_for_class(cls); return True
    except x509.ExtensionNotFound: return False

def san_len(c):
    return len(c.extensions.get_extension_for_class(x509.SubjectAlternativeName).value.public_bytes()) if has_ext(c, x509.SubjectAlternativeName) else 0

def want_parse(c, sigcode):
    pk = c.public_key()
    typ = 1 if isinstance(pk, rsa.RSAPublicKey) else 2
    curve = 0 if typ == 1 else (1 if isinstance(pk, ec.EllipticCurvePublicKey) and isinstance(pk.curve, ec.SECP256R1) else 2)
    ca = int(has_ext(c, x509.BasicConstraints) and c.extensions.get_extension_for_class(x509.BasicConstraints).value.ca)
    ku = c.extensions.get_extension_for_class(x509.KeyUsage).value if has_ext(c, x509.KeyUsage) else None
    kcs = int(bool(ku and ku.key_cert_sign))
    eku = int(has_ext(c, x509.ExtendedKeyUsage))
    return (f"type={typ} curve={curve} sig={sigcode} ca={ca} kcs={kcs} eku={eku} "
            f"san={san_len(c)} cn={cn_of(c)} nb={ts(c.not_valid_before_utc)} na={ts(c.not_valid_after_utc)}")

cases, exp = [], []
def add(line, want):
    cases.append(line); exp.append(want)

# ---------------------------------------------------------------- material
rk   = rsa.generate_private_key(public_exponent=65537, key_size=2048)
ek   = ec.generate_private_key(ec.SECP256R1())
ek3  = ec.generate_private_key(ec.SECP384R1())
root   = make("Test Root", "Test Root", rk.public_key(), rk, ca=True)
rootEC = make("EC Root", "EC Root", ek.public_key(), ek, ca=True)
rootP  = make("P Root", "P Root", ek3.public_key(), ek3, ca=True)
root384 = make("RSA384 Root", "RSA384 Root", rk.public_key(), rk, ca=True, alg=hashes.SHA384())

ik  = ec.generate_private_key(ec.SECP256R1())
inter = make("Test Intermediate", "Test Root", ik.public_key(), rk, ca=True)

ik2 = rsa.generate_private_key(public_exponent=65537, key_size=2048)
inter2 = make("RSA Intermediate", "Test Root", ik2.public_key(), rk, ca=True)

lk = rsa.generate_private_key(public_exponent=65537, key_size=2048)
leaf       = make("leaf.example", "Test Intermediate", lk.public_key(), ik, ca=False, san=["leaf.example", "www.leaf.example"], serial=5)
leaf_rsa   = make("direct.example", "Test Root", lk.public_key(), rk, ca=False, san=["direct.example"])
leaf_ec    = make("ecl.example", "EC Root", lk.public_key(), ek, ca=False, san=["ecl.example"])
leaf_p384  = make("p384.example", "P Root", lk.public_key(), ek3, ca=False, san=["p384.example"], alg=hashes.SHA384())
leaf_pss   = make("pss.example", "Test Root", lk.public_key(), rk, ca=False, san=["pss.example"], pss=True)
leaf_pss384= make("pss3.example", "RSA384 Root", lk.public_key(), rk, ca=False, san=["pss3.example"], alg=hashes.SHA384(), pss=True)
leaf_cn    = make("fallback.example", "Test Root", lk.public_key(), rk, ca=False, san=None)
leaf_wild  = make("wild.example", "Test Root", lk.public_key(), rk, ca=False, san=["*.example.com"])
leaf_multi = make("multi.example", "Test Root", lk.public_key(), rk, ca=False, san=["a.example", "b.example", "*.multi.example"])
leaf_exp   = make("exp.example", "Test Root", lk.public_key(), rk, ca=False, san=["exp.example"], na=D(2021, 1, 1))
leaf_ny    = make("ny.example", "Test Root", lk.public_key(), rk, ca=False, san=["ny.example"], nb=D(2030, 1, 1))
inter_exp  = make("Expired Intermediate", "Test Root", ik.public_key(), rk, ca=True, na=D(2021, 1, 1))
leaf_under_exp = make("u.example", "Expired Intermediate", lk.public_key(), ik, ca=False, san=["u.example"])

badk = ec.generate_private_key(ec.SECP256R1())
bad_inter = make("Bad CA", "Test Root", badk.public_key(), rk, ca=False)
leaf_bad  = make("bad.example", "Bad CA", lk.public_key(), badk, ca=False, san=["bad.example"])

# ---------------------------------------------------------------- parsing
add(f"parse {H(leaf)}",       [want_parse(leaf, 5)])         # RSA key, ECDSA-signed
add(f"parse {H(root)}",       [want_parse(root, 1)])
add(f"parse {H(inter)}",      [want_parse(inter, 1)])
add(f"parse {H(leaf_rsa)}",   [want_parse(leaf_rsa, 1)])
add(f"parse {H(leaf_ec)}",    [want_parse(leaf_ec, 5)])
add(f"parse {H(leaf_p384)}",  [want_parse(leaf_p384, 6)])
add(f"parse {H(leaf_pss)}",   [want_parse(leaf_pss, 3)])
add(f"parse {H(leaf_pss384)}",[want_parse(leaf_pss384, 4)])
add(f"parse {H(leaf_cn)}",    [want_parse(leaf_cn, 1)])
add(f"parse {H(root384)}",    [want_parse(root384, 2)])

# ---------------------------------------------------------------- hostname
add(f"host {H(leaf)} leaf.example",       ["match"])
add(f"host {H(leaf)} www.leaf.example",   ["match"])
add(f"host {H(leaf)} other.example",      ["no"])
add(f"host {H(leaf)} LEAF.EXAMPLE",       ["match"])
add(f"host {H(leaf)} sub.leaf.example",   ["no"])
add(f"host {H(leaf_cn)} fallback.example",["match"])
add(f"host {H(leaf_cn)} nope.example",    ["no"])
add(f"host {H(leaf_wild)} www.example.com",["match"])
add(f"host {H(leaf_wild)} example.com",   ["no"])
add(f"host {H(leaf_wild)} a.b.example.com",["no"])
add(f"host {H(leaf_wild)} x.WWW.example.com",["no"])
add(f"host {H(leaf_multi)} b.example",    ["match"])
add(f"host {H(leaf_multi)} x.multi.example",["match"])
add(f"host {H(leaf_multi)} y.x.multi.example",["no"])

# ---------------------------------------------------------------- chains
def chain(now=NOW, host="leaf.example", roots=None, ch=None):
    roots, ch = roots or [], ch or []
    return " ".join(["chain", str(now), host or "-", str(len(roots))] + [H(r) for r in roots]
                    + [str(len(ch))] + [H(c) for c in ch])

add(chain(host="leaf.example", roots=[root], ch=[leaf, inter]), ["OK"])
add(chain(host="direct.example", roots=[root], ch=[leaf_rsa]), ["OK"])
add(chain(host="ecl.example", roots=[rootEC], ch=[leaf_ec]), ["OK"])
add(chain(host="p384.example", roots=[rootP], ch=[leaf_p384]), ["OK"])
add(chain(host="pss.example", roots=[root], ch=[leaf_pss]), ["OK"])
add(chain(host="pss3.example", roots=[root384], ch=[leaf_pss384]), ["OK"])
add(chain(host="ecl.example", roots=[root, rootEC], ch=[leaf_ec]), ["OK"])   # one unused root present
add(chain(host="leaf.example", roots=[], ch=[leaf, inter]), ["ERR -8"])
add(chain(host="wrong.example", roots=[root], ch=[leaf, inter]), ["ERR -4"])
add(chain(host=None, roots=[root], ch=[leaf, inter]), ["OK"])
add(chain(host="exp.example", roots=[root], ch=[leaf_exp]), ["ERR -3"])
add(chain(host="ny.example", roots=[root], ch=[leaf_ny]), ["ERR -2"])
add(chain(host="leaf.example", roots=[root], ch=[leaf]), ["ERR -8"])     # missing intermediate
add(chain(host="u.example", roots=[root], ch=[leaf_under_exp, inter_exp]), ["ERR -3"])
add(chain(host="direct.example", roots=[rootEC], ch=[leaf_rsa]), ["ERR -8"])   # wrong root
add(chain(host="leaf.example", roots=[root], ch=[leaf, inter, root]), ["OK"])  # extra unrelated cert

# non-CA intermediate
badA = make("Fake CA", "Test Root", ik.public_key(), rk, ca=False)
badLeaf = make("badca.example", "Fake CA", lk.public_key(), ik, ca=False, san=["badca.example"])
add(chain(host="badca.example", roots=[root], ch=[badLeaf, badA]), ["ERR -7"])

# tampered leaf signature and tampered intermediate signature
tb = bytearray(leaf.public_bytes(serialization.Encoding.DER)); tb[-1] ^= 1
add(chain(host="leaf.example", roots=[root], ch=[bytes(tb), inter]), ["ERR -8"])
ti = bytearray(inter.public_bytes(serialization.Encoding.DER)); ti[-1] ^= 1
add(chain(host="leaf.example", roots=[root], ch=[leaf, bytes(ti)]), ["ERR -8"])

# a different root key that happens to use the same subject name must not verify
evil = make("Test Root", "Test Root", rsa.generate_private_key(public_exponent=65537, key_size=2048).public_key(), rk, ca=True)
add(chain(host="leaf.example", roots=[evil], ch=[leaf, inter]), ["ERR -8"])

# ---------------------------------------------------------------- PEM
pem1 = root.public_bytes(serialization.Encoding.PEM)
add(f"pem {pem1.hex()}", [f"blocks=1 len={len(root.public_bytes(serialization.Encoding.DER))} parse=0"])
pem3 = root.public_bytes(serialization.Encoding.PEM) + rootEC.public_bytes(serialization.Encoding.PEM) + rootP.public_bytes(serialization.Encoding.PEM)
add(f"pem {pem3.hex()}", [f"blocks=3 len={len(root.public_bytes(serialization.Encoding.DER))} parse=0"])
add(f"pem {'00'*40}", ["PEMBAD"])

# ---------------------------------------------------------------- run
inp = "\n".join(cases) + "\n"
r = subprocess.run([CLI], input=inp, capture_output=True, text=True)
out = r.stdout.split("\n")
pos, fails = 0, 0
for line, want in zip(cases, exp):
    got = out[pos]; pos += 1
    if got != want[0]:
        fails += 1
        print(f"FAIL: {line[:70]}\n   got  {got}\n   want {want[0]}")
print(f"{len(cases)-fails}/{len(cases)} X.509 cases passed")
sys.exit(1 if fails else 0)
