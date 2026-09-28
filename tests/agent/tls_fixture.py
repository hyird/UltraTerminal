"""Native TLS fixture. No HTTP/WebSocket framing and no operating-system input."""
import asyncio
import pathlib
import ssl
import struct
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))
from certificate import certificate


async def run(executable, directory):
    cert, key = certificate(directory)
    other = directory / "untrusted"
    other.mkdir()
    untrusted, _ = certificate(other)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.load_cert_chain(cert, key)
    context.set_alpn_protocols(["ultraterimal/1"])
    outcomes = []

    async def record(reader):
        size = struct.unpack("!I", await reader.readexactly(4))[0]
        assert 0 < size <= 65536, size
        return await reader.readexactly(size)

    async def peer(reader, writer):
        try:
            assert writer.get_extra_info("ssl_object").selected_alpn_protocol() == "ultraterimal/1"
            mode = (await record(reader)).decode()
            if mode in ("success", "slow_progress", "idle_header"):
                data = await record(reader)
                assert data == bytes(i % 251 for i in range(65536))
                packet = struct.pack("!I", len(data)) + data + struct.pack("!I", 4) + b"tail"
                if mode == "idle_header":
                    await asyncio.sleep(11)
                if mode == "slow_progress":
                    writer.write(struct.pack("!I", len(data)))
                    for index in range(4):
                        if index:
                            await asyncio.sleep(4)
                        writer.write(data[index * 16384:(index + 1) * 16384])
                        await writer.drain()
                    writer.write(struct.pack("!I", 4) + b"tail")
                    await writer.drain()
                    outcomes.append(mode)
                    return
                # Split the length itself, then combine two complete records.
                for part in (packet[:1], packet[1:3], packet[3:1024], packet[1024:]):
                    writer.write(part)
                    await writer.drain()
                    await asyncio.sleep(0.005)
                outcomes.append(mode)
            elif mode in ("oversize", "zero"):
                writer.write(struct.pack("!I", 65537 if mode == "oversize" else 0))
                await writer.drain()
                outcomes.append(mode)
            elif mode in ("truncated", "stalled_body"):
                writer.write(struct.pack("!I", 50) + b"partial")
                await writer.drain()
                outcomes.append(mode)
                if mode == "stalled_body": await reader.read()
            elif mode == "cancel":
                await reader.read()
                outcomes.append(mode)
            elif mode == "stalled_header":
                writer.write(b"\x00")
                await writer.drain()
                outcomes.append(mode)
                await reader.read()
            elif mode == "trickle_body":
                writer.write(struct.pack("!I", 120))
                outcomes.append(mode)
                for _ in range(120):
                    writer.write(b"x")
                    await writer.drain()
                    await asyncio.sleep(1)
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        except Exception as error:
            outcomes.append(error)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, ssl.SSLError):
                pass

    async def client(server, mode, trust=cert, host="localhost"):
        process = await asyncio.create_subprocess_exec(executable, str(trust), host, str(server.sockets[0].getsockname()[1]), mode,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        try:
            timeout = 75 if mode == "trickle_body" else 20 if mode in ("stalled_body", "stalled_header", "slow_progress", "idle_header") else 8
            stdout, stderr = await asyncio.wait_for(process.communicate(), timeout)
        except asyncio.TimeoutError:
            process.kill()
            await process.wait()
            raise AssertionError(f"Native TLS hung: {mode}")
        assert process.returncode == 0, (mode, stdout, stderr)
        print(stdout.decode().strip())

    modes = ("trickle_body",) if len(sys.argv) > 2 and sys.argv[2] == "deadline" else ("success", "slow_progress", "idle_header", "oversize", "zero", "truncated", "stalled_body", "stalled_header", "cancel")
    async with await asyncio.start_server(peer, "127.0.0.1", 0, ssl=context) as server:
        for mode in modes:
            await client(server, mode)
        if modes == ("trickle_body",):
            assert set(outcomes) == set(modes), outcomes
            return
        await client(server, "hostname", host="127.0.0.1")
        await client(server, "untrusted", trust=untrusted)
    assert set(outcomes) == set(modes), outcomes

    async def silent(reader, writer):
        try:
            await reader.read()
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, ssl.SSLError):
                pass

    context.set_alpn_protocols(["http/1.1"])
    async with await asyncio.start_server(silent, "127.0.0.1", 0, ssl=context) as server:
        await client(server, "alpn")
    async with await asyncio.start_server(silent, "127.0.0.1", 0) as server:
        await client(server, "timeout", host="127.0.0.1")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="ultraterimal-native-tls-") as folder:
        asyncio.run(run(sys.argv[1], pathlib.Path(folder)))
