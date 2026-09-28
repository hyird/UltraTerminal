"""Run the production gateway lifecycle with a verified loopback QUIC client."""
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))
from certificate import certificate

with tempfile.TemporaryDirectory(prefix="ultraterimal-shutdown-") as folder:
    cert, key = certificate(pathlib.Path(folder))
    subprocess.run([sys.argv[1], str(cert), str(key)], check=True, timeout=15)
