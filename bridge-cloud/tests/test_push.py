from __future__ import annotations

import os

import pytest
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey

from grafana_push.push import seal, unseal


def _keypair() -> tuple[bytes, bytes]:
    sk = X25519PrivateKey.generate()
    return sk.private_bytes_raw(), sk.public_key().public_bytes_raw()


def test_seal_roundtrip_small() -> None:
    sk, pk = _keypair()
    sealed = seal(pk, b"hello")
    assert unseal(sk, sealed) == b"hello"


def test_seal_roundtrip_bundle_sized() -> None:
    sk, pk = _keypair()
    pt = os.urandom(5000)
    sealed = seal(pk, pt)
    # epk(32) + nonce(12) + plaintext + tag(16)
    assert len(sealed) == 32 + 12 + len(pt) + 16
    assert unseal(sk, sealed) == pt


def test_seal_produces_distinct_ciphertexts() -> None:
    """Same plaintext + recipient → different ciphertext each call, because
    the ephemeral keypair and nonce are random."""
    _, pk = _keypair()
    s1 = seal(pk, b"identical")
    s2 = seal(pk, b"identical")
    assert s1 != s2


def test_seal_rejects_wrong_recipient_key_size() -> None:
    with pytest.raises(ValueError):
        seal(b"too short", b"data")


def test_unseal_fails_on_tampered_ciphertext() -> None:
    sk, pk = _keypair()
    sealed = bytearray(seal(pk, b"payload"))
    # Flip a bit in the ciphertext body.
    sealed[-20] ^= 0x01
    with pytest.raises(Exception):
        unseal(sk, bytes(sealed))


def test_unseal_fails_with_wrong_recipient() -> None:
    _, pk = _keypair()
    other_sk, _ = _keypair()
    sealed = seal(pk, b"payload")
    with pytest.raises(Exception):
        unseal(other_sk, sealed)
