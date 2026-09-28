"""Independent TLS peer for the Agent's desktop channel boundary and cancellation."""
import asyncio
import json
import pathlib
import ssl
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))
from certificate import certificate
from wire_fixture import authenticate, binary, envelope, fields, number, record, read_record


def packet(purpose, sequence, field, body=b'', session=b'session'):
    return envelope(sequence, field, body, session=session) + binary(3, b'input' if purpose == 2 else b'video')


async def run_mode(executable, folder, context, cert, mode, worker=None):
    tasks = set()
    errors = []
    opened = set()
    both_open = asyncio.Event()
    first_frame = asyncio.Event()
    observed = set()

    async def peer(reader, writer):
        task = asyncio.current_task()
        tasks.add(task)
        try:
            await authenticate(reader, writer, 2)
            data = await read_record(reader)
            message = fields(data)
            grant = fields(message[16])
            purpose = grant[1]
            assert message[4] == 1 and message[2] == b'session'
            assert purpose in (2, 3) and purpose not in opened
            assert grant[2] == (b'input-ticket' if purpose == 2 else b'video-ticket')
            opened.add(purpose)
            if len(opened) == 2: both_open.set()
            await both_open.wait()
            if mode == 'cancel_pair':
                await reader.read()
                observed.add('cancelled')
                return
            readiness = packet(purpose, 1, 29, number(1, purpose), session=b'other' if mode == 'wrong_ready' else b'session')
            writer.write(record(readiness))
            await writer.drain()
            if mode == 'wrong_ready':
                observed.add('wrong_ready')
                await reader.read()
                return
            if purpose == 2:
                data = await read_record(reader)
                assert fields(data)[4] == 2 and 23 in fields(data)
                await first_frame.wait()
                if mode == 'writable':
                    writer.write(record(packet(2, 2, 22, number(1, 5)) + number(5, 1)))
                payload = packet(2, 2, 22, number(1, 3) + number(4, 65)) if mode == 'read_only' else packet(2, 2, 25)
                if mode == 'writable': payload = packet(2, 3, 25)
                if mode == 'wrong_input_binding': payload = packet(2, 2, 25, session=b'other')
                writer.write(record(payload))
                await writer.drain()
                observed.add('input_sent')
                await reader.read()
            else:
                data = await read_record(reader)
                decoded = fields(data)
                assert decoded[4] == 2 and fields(decoded[18])[1].startswith(b'avc1.')
                if worker:
                    assert fields(decoded[18])[2] > 32 and fields(decoded[18])[3] > 32
                data = await read_record(reader)
                decoded = fields(data)
                assert decoded[6] == 1 and fields(decoded[19])[2] == 1
                assert fields(decoded[19])[1].startswith((b'\x00\x00\x00\x01', b'\x00\x00\x01'))
                timestamp = fields(decoded[19]).get(3, 0)
                first_frame.set()
                if mode in ('read_only', 'wrong_input_binding'):
                    await reader.read()
                    return
                if worker:
                    previous_sequence = decoded[4]
                    previous_frame = decoded[6]
                    while True:
                        data = await read_record(reader)
                        decoded = fields(data)
                        assert decoded[4] > previous_sequence and decoded[6] > previous_frame
                        previous_sequence, previous_frame = decoded[4], decoded[6]
                        assert fields(decoded[19])[3] > timestamp
                        if fields(decoded[19]).get(2): break
                else:
                    data = await read_record(reader)
                    decoded = fields(data)
                    assert decoded[4] == 4 and decoded[6] == 2 and fields(decoded[19])[3] == 33333
                observed.add('refreshed')
                writer.close()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        except Exception as error:
            errors.append(error)
        finally:
            writer.close()
            try: await writer.wait_closed()
            except (ConnectionError, ssl.SSLError): pass
            tasks.discard(task)

    async with await asyncio.start_server(peer, '127.0.0.1', 0, ssl=context) as server:
        config = folder / f'{mode}.json'
        config.write_text(json.dumps({'host': 'localhost', 'address': '127.0.0.1', 'port': str(server.sockets[0].getsockname()[1]),
            'device_id': 'fixture-device', 'ca_file': str(cert), 'credential_file': str(folder / f'{mode}.credential')}), encoding='utf8')
        session = number(1, 1) + binary(2, b'user')
        if mode == 'writable': session += number(5, 1) + number(7, 2)
        for purpose, channel, ticket in ((2, b'input', b'input-ticket'), (3, b'video', b'video-ticket')):
            session += binary(6, binary(1, channel) + number(2, purpose) + binary(3, ticket))
        arguments = ['--bridge', str(worker)] if worker else ['--fixture-cancel' if mode == 'cancel_pair' else '--fixture']
        process = await asyncio.create_subprocess_exec(str(executable), str(config), *arguments,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(envelope(1, 13, session, session=b'session')), 10)
            assert process.returncode == (0 if mode == 'success' and not worker else 1), (stdout + stderr).decode(errors='replace')
            if worker:
                assert b'Bridge disconnect joined desktop child' in stdout, (stdout + stderr).decode(errors='replace')
            if tasks: await asyncio.wait_for(asyncio.gather(*tuple(tasks)), 3)
        finally:
            if process.returncode is None:
                process.kill()
                await process.wait()
            for task in tuple(tasks): task.cancel()
            if tasks: await asyncio.gather(*tuple(tasks), return_exceptions=True)
    assert not errors, errors
    assert opened == {2, 3}, f'{mode}: both Agent connections were not exercised'
    expected = 'refreshed' if mode in ('success', 'writable') else 'cancelled' if mode == 'cancel_pair' else 'wrong_ready' if mode == 'wrong_ready' else 'input_sent'
    assert expected in observed, f'{mode}: missing peer evidence'
    print(f'Desktop transport {mode} passed')


async def run(executable, folder, worker=None):
    cert, key = certificate(folder)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.set_alpn_protocols(["ultraterimal/1"])
    modes = ('success', 'writable', 'read_only', 'wrong_input_binding') if worker else ('success', 'wrong_ready', 'read_only', 'wrong_input_binding', 'cancel_pair')
    for mode in modes:
        await run_mode(executable, folder, context, cert, mode, worker)


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='ultraterimal-desktop-') as temporary:
        asyncio.run(run(pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(temporary), pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None))
