"""Encrypt with the C++ code using a test key, decrypt in Python: must give the plaintext back."""
import base64, json, random, subprocess, sys
from cryptography.hazmat.primitives.asymmetric import rsa, padding
from cryptography.hazmat.primitives import serialization
exe = sys.argv[1]
key = rsa.generate_private_key(public_exponent=65537, key_size=1024)
open(sys.argv[2], "wb").write(key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
rng = random.Random(3); ok = 0; N = 60
for i in range(N):
    plain = json.dumps({"memberId": i, "token": "x" * rng.randint(0, 40), "theName": "قهوة " * rng.randint(0, 30),
                        "pad": "y" * rng.randint(0, 700)}, ensure_ascii=False, separators=(",", ":")).encode()
    b64 = subprocess.run([exe, sys.argv[2]], input=plain, capture_output=True, check=True).stdout
    raw = base64.b64decode(b64)
    assert len(raw) % 128 == 0 and len(raw) // 128 == -(-len(plain) // 117)
    back = b"".join(key.decrypt(raw[k:k + 128], padding.PKCS1v15()) for k in range(0, len(raw), 128))
    ok += back == plain
print(f"encryption round trip: {ok}/{N} decrypt to the original")
real = subprocess.run([exe, sys.argv[2], "xbloom"], input=b'{"a":1}', capture_output=True, check=True).stdout
print("xBloom key loads and encrypts:", len(base64.b64decode(real)) == 128)
sys.exit(0 if ok == N else 1)
