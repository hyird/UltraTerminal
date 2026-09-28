"""Verify enrollment rejects untrusted TLS and incomplete or invalid identities."""
import asyncio
import json
import pathlib
import ssl
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))
from certificate import certificate
from wire_fixture import binary, fields, number, record, read_record


async def run(executable, folder):
    cert, key = certificate(folder)
    other = folder / 'untrusted'
    other.mkdir()
    wrong_ca, _ = certificate(other)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.set_alpn_protocols(['ultraterimal/1'])
    mode = ''
    received = []
    tasks = set()

    async def peer(reader, writer):
        task = asyncio.current_task()
        tasks.add(task)
        try:
            request = fields(await read_record(reader))
            assert request == {1:1, 2:3, 4:b'a' * 64, 5:b'fixture-agent'}
            received.append(mode)
            if mode == 'silent':
                await reader.read()
                return
            body = number(1, 2 if mode == 'version' else 1) + number(2, 1) + binary(3, b'd-fixture') + binary(4, b'b' * 64)
            if mode == 'rejected': body = number(1, 1)
            if mode == 'malformed': body = b'not-protobuf'
            if mode == 'missing': body = number(1, 1) + number(2, 1) + binary(3, b'd-fixture')
            if mode == 'identity': body = number(1, 1) + number(2, 1) + binary(3, b'bad\r\nidentity') + binary(4, b'b' * 64)
            if mode == 'credential': body = number(1, 1) + number(2, 1) + binary(3, b'd-fixture') + binary(4, b'secret-invalid')
            if mode == 'oversized': body = b'x' * 4097
            response = record(body)
            writer.write(response[:-10] if mode == 'truncated' else response)
            await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()
            try: await writer.wait_closed()
            except (OSError, ssl.SSLError): pass
            tasks.discard(task)

    async with await asyncio.start_server(peer, '127.0.0.1', 0, ssl=context) as server:
        port = str(server.sockets[0].getsockname()[1])
        for mode in ('hostname','untrusted','version','rejected','malformed','missing','identity','credential','oversized','truncated','invalid-input','silent'):
            config = folder / (mode + '.json')
            credential = folder / (mode + '.credential')
            config.write_text(json.dumps({'host':'127.0.0.1' if mode == 'hostname' else 'localhost','address':'127.0.0.1','port':port,
                                          'ca_file':str(wrong_ca if mode == 'untrusted' else cert),'credential_file':str(credential)}), encoding='utf8')
            payload = json.dumps({'code':'a' * 64,'name':'fixture-agent','unexpected':True} if mode == 'invalid-input' else {'code':'a' * 64,'name':'fixture-agent'}).encode()
            started = asyncio.get_running_loop().time()
            process = await asyncio.create_subprocess_exec(str(executable), '--enroll', str(config), stdin=asyncio.subprocess.PIPE,
                                                          stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            try:
                output, error = await asyncio.wait_for(process.communicate(payload), timeout=15)
            except asyncio.TimeoutError:
                process.kill()
                await process.wait()
                raise AssertionError('Enrollment deadline failed in ' + mode)
            assert process.returncode == 1 and not output and not credential.exists(), mode
            if mode == 'silent':
                assert 9 <= asyncio.get_running_loop().time() - started < 15, 'Enrollment did not honor the request deadline'
            assert b'a' * 64 not in error and b'b' * 64 not in error and b'secret-invalid' not in error, mode
            print(mode, 'rejected')
        assert not any(mode in received for mode in ('hostname','untrusted','invalid-input'))
        assert len(received) == 9, received
    if tasks:
        await asyncio.wait_for(asyncio.gather(*tasks), timeout=3)


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='ultraterimal-enrollment-') as directory:
        asyncio.run(run(pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(directory)))
