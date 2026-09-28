"""Ephemeral test-only P-256 credentials; fixed dates, no production material."""
from pathlib import Path
from datetime import datetime, timezone
import sys
from cryptography import x509
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
key = ec.generate_private_key(ec.SECP256R1())
ca_key = ec.generate_private_key(ec.SECP256R1())
other = ec.generate_private_key(ec.SECP256R1())
def name(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
issuer = name('TianShan isolated test CA')
def cert(label, public, start, end, serial, ca=False):
    c = (x509.CertificateBuilder().subject_name(issuer if ca else name('localhost'))
         .issuer_name(issuer).public_key(public.public_key()).serial_number(serial)
         .not_valid_before(datetime.fromisoformat(start).replace(tzinfo=timezone.utc))
         .not_valid_after(datetime.fromisoformat(end).replace(tzinfo=timezone.utc))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
         .add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost')]), critical=False))
    if not ca: c = c.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH, ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
    out.joinpath(label+'.pem').write_bytes(c.sign(ca_key, hashes.SHA256()).public_bytes(serialization.Encoding.PEM))
cert('ca',ca_key,'2020-01-01','2099-01-01',1,True)
cert('a',key,'2026-01-01','2027-01-01',2)
cert('b',key,'2026-01-01','2027-01-01',3)
cert('future',key,'2050-01-01','2051-01-01',4)
cert('expired',key,'2020-01-01','2021-01-01',5)
cert('wrong',other,'2026-01-01','2027-01-01',6)
out.joinpath('key.pem').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
