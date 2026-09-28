"""Exercise the production control runtime against a bounded loopback TLS peer."""
import asyncio
import pathlib
import ssl
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))
from certificate import certificate
from wire_fixture import authenticate, binary, envelope, fields, number, record, read_record


async def run_mode(executable, directory, context, cert, mode, agent=None):
    errors = []
    tasks = set()
    connections = 0
    acknowledgments = 0
    rejected = 0
    connection_times = []
    desktop_opened = set()
    desktop_ready = asyncio.Event()
    desktop_frame = asyncio.Event()
    desktop_refreshed = asyncio.Event()
    desktop_closed = False
    desktop_mode = mode.startswith('desktop_')

    async def desktop_peer(reader, writer):
        data = await read_record(reader)
        opened = fields(data)
        purpose = fields(opened[16])[1]
        assert opened[2] == b'fixture-session' and purpose in (2, 3)
        assert purpose not in desktop_opened
        desktop_opened.add(purpose)
        if len(desktop_opened) == 2: desktop_ready.set()
        await desktop_ready.wait()
        def packet(sequence, field, body=b''):
            return envelope(sequence, field, body, b'fixture-session') + binary(3, opened[3])
        writer.write(record(packet(1, 29, number(1, purpose))))
        await writer.drain()
        if purpose == 2:
            data = await read_record(reader)
            assert 23 in fields(data)
            await desktop_frame.wait()
            writer.write(record(packet(2, 25)))
            await writer.drain()
            sequence = 2
            while True:
                message = fields(await read_record(reader))
                if 36 in message:
                    sequence += 1
                    writer.write(record(packet(sequence, 36, message[36])))
                    await writer.drain()
                else:
                    assert 23 in message
        else:
            data = await read_record(reader)
            config = fields(fields(data)[18])
            assert config[2] > 32 and config[3] > 32
            first = True
            while True:
                data = await read_record(reader)
                message = fields(data)
                assert 19 in message
                image = fields(message[19])
                assert image[1].startswith((b'\x00\x00\x00\x01', b'\x00\x00\x01'))
                if first:
                    assert image[2] == 1
                    first = False
                    desktop_frame.set()
                elif image.get(2) == 1:
                    desktop_refreshed.set()
                    await reader.read()
                    return

    async def peer(reader, writer):
        nonlocal connections, acknowledgments, rejected, desktop_closed
        task = asyncio.current_task()
        tasks.add(task)
        try:
            connection_times.append(asyncio.get_running_loop().time())
            if mode == "stop_connect":
                await reader.read()
                return
            if mode == "stop_backoff":
                return
            purpose = await authenticate(reader, writer)
            if purpose == 2:
                assert desktop_mode
                await desktop_peer(reader, writer)
                return
            data = await read_record(reader)

            hello = fields(data)
            assert hello[4] == 1 and fields(hello[10])[1] == b"fixture-device"
            if desktop_mode:
                capabilities = fields(fields(hello[10])[3])
                assert capabilities[2] == 1 and capabilities[3] == 1 and capabilities[12] == 2
            connections += 1
            connection = connections
            if mode == "no_welcome":
                await reader.read()
                return
            writer.write(record(envelope(1, 11, number(1, 1))))
            await writer.drain()
            server_sequence = 1
            if (mode == "reconnect" and connection == 2) or (desktop_mode and connection == 1):
                server_sequence += 1
                grants = b''.join(binary(6, binary(1, channel) + number(2, purpose) + binary(3, b"fixture-ticket"))
                                  for channel, purpose in ((b'input-channel', 2), (b'video-channel', 3)))
                session = number(1, 1) + binary(2, b"fixture-user") + grants
                writer.write(record(envelope(server_sequence, 13, session, b"fixture-session")))
                await writer.drain()
            previous = 1
            while True:
                data = await read_record(reader)

                message = fields(data)
                assert message[4] > previous
                previous = message[4]
                if 15 in message:
                    assert message[2] == b"fixture-session"
                    assert fields(message[15])[1] == b"capability_unavailable"
                    rejected += 1
                elif 14 in message:
                    assert mode == 'desktop_session' and message[2] == b'fixture-session'
                    desktop_closed = True
                else:
                    assert 12 in message
                    if mode == "silent_heartbeat":
                        continue
                    if mode == "replay":
                        writer.write(record(envelope(1, 12)))
                        await writer.drain()
                        await reader.read()
                        return
                    server_sequence += 1
                    writer.write(record(envelope(server_sequence, 12)))
                    await writer.drain()
                    acknowledgments += 1
                    if mode == 'desktop_session' and desktop_refreshed.is_set() and not desktop_closed:
                        server_sequence += 1
                        writer.write(record(envelope(server_sequence, 14, binary(1, b'completed'), b'fixture-session')))
                        await writer.drain()
                    if connection == 1 and mode == 'desktop_disconnect' and desktop_refreshed.is_set():
                        return
                    if connection == 1 and not desktop_mode:
                        return  # Simulate loss after a successful heartbeat.
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        except Exception as error:
            errors.append(error)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, ssl.SSLError):
                pass
            tasks.discard(task)

    async with await asyncio.start_server(peer, "127.0.0.1", 0,
                                          ssl=None if mode in ("stop_connect", "stop_backoff") else context) as server:
        port = str(server.sockets[0].getsockname()[1])
        process = await asyncio.create_subprocess_exec(executable, str(cert), port, str(directory / (mode + ".credential")), mode, *([str(agent)] if agent else []),
                                                       stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(), timeout=20)
        except asyncio.TimeoutError:
            process.kill()
            await process.wait()
            raise AssertionError(f"Control runtime hung in {mode}")
        if tasks:
            await asyncio.wait_for(asyncio.gather(*list(tasks)), timeout=2)
        assert process.returncode == 0, (mode, stdout, stderr)
        assert not errors, errors
        if mode == "reconnect":
            assert connections >= 2 and acknowledgments >= 3 and rejected >= 1
            assert connection_times[1] - connection_times[0] >= 0.2
        if mode == "silent_heartbeat":
            assert connections >= 2
            assert connection_times[1] - connection_times[0] >= 2.2
        if mode == "no_welcome":
            assert connections >= 2
            assert connection_times[1] - connection_times[0] >= 1.2
        if mode == "replay":
            assert connections >= 2
        if desktop_mode:
            assert desktop_opened == {2, 3} and desktop_refreshed.is_set()
            assert desktop_closed == (mode == 'desktop_session')
        print(stdout.decode().strip())


async def run(executable, directory, agent=None):
    cert, key = certificate(directory)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.set_alpn_protocols(["ultraterimal/1"])
    for mode in (('desktop_session', 'desktop_disconnect', 'desktop_stop') if agent else ("reconnect", "silent_heartbeat", "no_welcome", "stop_connect", "stop_backoff", "replay")):
        await run_mode(executable, directory, context, cert, mode, agent)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="ultraterimal-control-") as folder:
        asyncio.run(run(sys.argv[1], pathlib.Path(folder), sys.argv[2] if len(sys.argv) > 2 else None))
