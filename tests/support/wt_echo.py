#!/usr/bin/env python3
# Minimal WebTransport (HTTP/3) datagram echo server for the wasm net::web WebTransport E2E test. Generates a
# short-lived ECDSA P-256 self-signed cert (what serverCertificateHashes requires for local testing), prints
# its SHA-256 as `CERTHASH <hex>` on stdout so the browser harness can trust it, then echoes every datagram
# on the WebTransport session. Depends only on aioquic + cryptography (both in the dev image). Usage:
#   python3 wt_echo.py [port]
import asyncio, datetime, hashlib, ipaddress, os, sys, tempfile

from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

from aioquic.asyncio import serve
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3Connection, H3_ALPN
from aioquic.h3.events import HeadersReceived, DatagramReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ProtocolNegotiated

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 47680


def make_cert():
    key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.datetime.now(datetime.timezone.utc)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    cert = (x509.CertificateBuilder()
            .subject_name(name).issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(days=10))   # serverCertificateHashes: <= 14 days
            .add_extension(x509.SubjectAlternativeName(
                [x509.DNSName("localhost"), x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), critical=False)
            .sign(key, hashes.SHA256()))
    der = cert.public_bytes(serialization.Encoding.DER)
    d = tempfile.mkdtemp()
    cpath, kpath = os.path.join(d, "cert.pem"), os.path.join(d, "key.pem")
    with open(cpath, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(kpath, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                  serialization.NoEncryption()))
    return cpath, kpath, hashlib.sha256(der).hexdigest()


class WTProtocol(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = None

    def quic_event_received(self, event):
        if isinstance(event, ProtocolNegotiated):
            self._http = H3Connection(self._quic, enable_webtransport=True)
        if self._http is not None:
            for h3 in self._http.handle_event(event):
                self._h3(h3)

    def _h3(self, event):
        if isinstance(event, HeadersReceived):
            headers = dict(event.headers)
            if headers.get(b":method") == b"CONNECT" and headers.get(b":protocol") == b"webtransport":
                self._http.send_headers(stream_id=event.stream_id, headers=[(b":status", b"200")])
                self.transmit()
        elif isinstance(event, DatagramReceived):
            self._http.send_datagram(event.stream_id, event.data)   # echo on the same session
            self.transmit()


async def main():
    cpath, kpath, cert_hash = make_cert()
    print("CERTHASH " + cert_hash, flush=True)
    config = QuicConfiguration(is_client=False, alpn_protocols=H3_ALPN, max_datagram_frame_size=65536)
    config.load_cert_chain(cpath, kpath)
    await serve("127.0.0.1", PORT, configuration=config, create_protocol=WTProtocol)
    sys.stderr.write("wt_echo listening on 127.0.0.1:%d\n" % PORT)
    sys.stderr.flush()
    await asyncio.Future()   # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
