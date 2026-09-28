"""Exercise a UDP blackhole and TCP fallback on the same loopback port."""
import asyncio
import pathlib
import ssl
import struct
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))
from certificate import certificate


async def run(executable, folder):
    cert, key = certificate(folder)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.load_cert_chain(cert, key)
    context.set_alpn_protocols(["ultraterimal/1"])
    connections = []
    packets = []

    class Blackhole(asyncio.DatagramProtocol):
        def datagram_received(self, data, address):
            packets.append(len(data))

    async def peer(reader, writer):
        connections.append(True)
        try:
            assert writer.get_extra_info("ssl_object").selected_alpn_protocol() == "ultraterimal/1"
            header = await reader.readexactly(4)
            length = struct.unpack("!I", header)[0]
            assert length == 13
            data = await reader.readexactly(length)
            assert data == b"native\0binary"
            writer.write(header + data)
            await writer.drain()
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, ssl.SSLError):
                pass

    # Windows reserves different TCP and UDP ranges. Keep both bound once selected.
    for _ in range(512):
        server = await asyncio.start_server(peer, "127.0.0.1", 0, ssl=context)
        port = server.sockets[0].getsockname()[1]
        try:
            udp, _ = await asyncio.get_running_loop().create_datagram_endpoint(Blackhole, local_addr=("127.0.0.1", port))
        except OSError as error:
            server.close()
            await server.wait_closed()
            if error.errno not in (13, 48, 98, 10013, 10048):
                raise
            continue
        break
    else:
        raise RuntimeError("Cannot bind a shared TCP/UDP fixture port")
    async with server:
        try:
            for mode in ("fallback", "cancel"):
                process = await asyncio.create_subprocess_exec(executable, str(cert), str(port), mode,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
                try:
                    stdout, stderr = await asyncio.wait_for(process.communicate(), 8)
                except asyncio.TimeoutError:
                    process.kill()
                    await process.wait()
                    raise AssertionError(f"Native connect hung: {mode}")
                assert process.returncode == 0, (stdout, stderr)
                print(stdout.decode().strip())
            assert len(connections) == 1, "Cancellation opened an unwanted TCP connection"
            assert len(packets) >= 2, "UDP was never attempted"
        finally:
            udp.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="ultraterimal-native-") as folder:
        asyncio.run(run(sys.argv[1], pathlib.Path(folder)))
