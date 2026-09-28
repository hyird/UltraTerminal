"""Bounded production QUIC client test against an independent TLS peer."""
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))
from certificate import certificate

with tempfile.TemporaryDirectory(prefix="ultraterimal-quic-") as folder:
    cert, key = certificate(pathlib.Path(folder))
    if len(sys.argv) == 3:
        subprocess.run([sys.argv[1], str(cert), str(key), sys.argv[2]], check=True, timeout=75)
        sys.exit(0)
    subprocess.run([sys.argv[1], str(cert), str(key)], check=True, timeout=12)
    subprocess.run([sys.argv[1], str(cert), str(key), "server-close"], check=True, timeout=12)
    subprocess.run([sys.argv[1], str(cert), str(key), "impaired"], check=True, timeout=12)
    subprocess.run([sys.argv[1], str(cert), str(key), "sustained-loss"], check=True, timeout=105)
    subprocess.run([sys.argv[1], str(cert), str(key), "stalled-body"], check=True, timeout=15)
    subprocess.run([sys.argv[1], str(cert), str(key), "stalled-header"], check=True, timeout=15)
    subprocess.run([sys.argv[1], str(cert), str(key), "idle-header"], check=True, timeout=15)
