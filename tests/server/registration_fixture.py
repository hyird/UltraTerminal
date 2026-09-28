"""Real Ruvia + PostgreSQL + Agent integration in a private temporary cluster."""
import ctypes
import faulthandler
import contextlib
import base64
import hashlib
import http.client
import json
import os
import pathlib
import secrets
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from ctypes import wintypes

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))
from certificate import certificate
from wire_fixture import binary, envelope, fields, number, record


class ControlBusy(Exception):
    pass


def agent_children(parent_pid):
    """Read the fixture Agent's direct children without inspecting other command lines."""
    class ProcessEntry(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('usage', wintypes.DWORD), ('pid', wintypes.DWORD),
                    ('heap', ctypes.c_size_t), ('module', wintypes.DWORD), ('threads', wintypes.DWORD),
                    ('parent', wintypes.DWORD), ('priority', wintypes.LONG), ('flags', wintypes.DWORD),
                    ('name', wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32FirstW.restype = kernel.Process32NextW.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = ProcessEntry()
        entry.size = ctypes.sizeof(entry)
        result = []
        available = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while available:
            if entry.parent == parent_pid and entry.name.lower() == 'agent.exe':
                result.append(entry.pid)
            available = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
            raise ctypes.WinError(ctypes.get_last_error())
        return result
    finally:
        kernel.CloseHandle(snapshot)


class BrowserPeer:
    """Independent wire peer exercising the production gateway's control boundary."""
    def __init__(self, context, port, *, cookie, origin):
        self.socket = context.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=5), server_hostname='localhost')
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        authentication = ''
        if cookie: authentication += f'Cookie: {cookie}\r\n'
        if origin: authentication += f'Origin: {origin}\r\n'
        self.socket.sendall((f'GET /api/channels/browser HTTP/1.1\r\nHost: localhost:{port}\r\n'
                             f'Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                             f'Sec-WebSocket-Key: {key}\r\n{authentication}\r\n').encode())
        header = b''
        while not header.endswith(b'\r\n\r\n'):
            header += self.exact(1)
            assert len(header) < 16384
        assert header.startswith(b'HTTP/1.1 101 '), 'Control upgrade rejected'
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        assert accept in header

    def exact(self, size):
        result = b''
        while len(result) < size:
            part = self.socket.recv(size - len(result))
            if not part: raise EOFError('Control peer closed')
            result += part
        return result

    def send(self, data, opcode=2):
        assert len(data) <= 16 * 1024 * 1024
        mask = secrets.token_bytes(4)
        length = bytes([0x80 | len(data)]) if len(data) < 126 else (b'\xfe' + struct.pack('!H', len(data)) if len(data) <= 65535 else b'\xff' + struct.pack('!Q', len(data)))
        self.socket.sendall(bytes([0x80 | opcode]) + length + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(data)))

    def receive(self):
        first, second = self.exact(2)
        assert first & 0x80 and not second & 0x80
        size = second & 127
        if size == 126: size = struct.unpack('!H', self.exact(2))[0]
        if size == 127: size = struct.unpack('!Q', self.exact(8))[0]
        assert size <= 16 * 1024 * 1024
        return first & 15, self.exact(size)

    def rejected(self):
        try:
            opcode, data = self.receive()
            assert opcode == 8, 'Rejected connection received application data'
            self.send(data, 8)
        except (EOFError, ConnectionError):
            pass

    def __enter__(self): return self
    def __exit__(self, *_): self.socket.close()


class NativePeer:
    """TLS record peer; BrowserPeer separately exercises CDN-facing WebSockets."""
    def __init__(self, context, port, credential, purpose=1):
        self.socket = context.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=5), server_hostname='localhost')
        assert self.socket.selected_alpn_protocol() == 'ultraterimal/1'
        self.send(number(1, 1) + number(2, purpose) + binary(3, credential.encode()))
        try:
            response = fields(self.receive()[1])
            assert response.get(1) == 1 and response.get(2) == 1 and response.get(3), 'Native authentication failed'
        except BaseException:
            self.socket.close()
            raise

    def exact(self, size):
        result = b''
        while len(result) < size:
            part = self.socket.recv(size - len(result))
            if not part: raise EOFError('Native peer closed')
            result += part
        return result

    def send(self, data):
        self.socket.sendall(record(data))

    def receive(self):
        size, = struct.unpack('!I', self.exact(4))
        assert 0 < size <= 16 * 1024 * 1024
        return 2, self.exact(size)

    def hello(self, device, capabilities=b''):
        self.send(envelope(1, 10, binary(1, device.encode()) + binary(2, b'fixture') + binary(3, capabilities)))
        try:
            data = self.receive()[1]
        except (EOFError, ConnectionError) as error:
            raise ControlBusy('Previous control lease is still being cleaned up') from error
        assert 11 in fields(data), 'Missing WELCOME'

    def rejected(self):
        try:
            self.receive()
        except (EOFError, ConnectionError):
            return
        raise AssertionError('Rejected connection received application data')

    def __enter__(self): return self
    def __exit__(self, *_): self.socket.close()


@contextlib.contextmanager
def connected_control(context, native_port, registered, capabilities=b''):
    # Offline means unavailable for new sessions; the old lease can still be
    # joining tasks and closing database sessions. Retry only that exact refusal.
    deadline = time.monotonic() + 5
    while True:
        peer = NativePeer(context, native_port, registered['credential'])
        try:
            peer.hello(registered['device_id'], capabilities)
            break
        except ControlBusy:
            peer.socket.close()
            if time.monotonic() >= deadline:
                raise AssertionError('Previous control lease did not finish cleanup')
            time.sleep(.02)
        except BaseException:
            peer.socket.close()
            raise
    with peer:
        yield peer


def desktop_channels(context, gateway_port, native_port, registered, cookie, origin, request, session_secrets, sql):
    with connected_control(context, native_port, registered, number(2, 1) + number(3, 1) + number(12, 2)) as control:
        control_sequence = 1
        for iteration, mode in enumerate(('disconnect', 'read_only', 'user_close', 'revoke')):
            writable = mode != 'read_only'
            control_sequence += 1
            control.send(envelope(control_sequence, 12))
            assert 12 in fields(control.receive()[1])
            status, _, session = request('POST', '/api/sessions',
                {'device_id': registered['device_id'], 'kind': 'desktop', 'write_control': writable,
                 **({'desktop_input_contract': 2} if writable else {})}, cookie, origin)
            assert status == 202
            opcode, command = control.receive()
            assert opcode == 2 and 13 in fields(command)
            opened = fields(fields(command)[13], repeated=True)
            assert opened.get(7, [0])[0] == (2 if writable else 0), 'Gateway issued the wrong desktop input contract'
            grants = {fields(grant)[2]: fields(grant) for grant in opened[6]}
            browser_grants = {grant['purpose']: grant for grant in session['channels']}
            session_secrets.extend(grant[3].decode() for grant in grants.values())
            session_secrets.extend(grant['ticket'] for grant in browser_grants.values())

            def packet(purpose, sequence, field, body=b'', generation=0, frame_id=0):
                return (number(1, 1) + binary(2, session['id'].encode()) + binary(3, browser_grants[purpose]['id'].encode()) +
                    number(4, sequence) + (number(5, generation) if generation else b'') +
                    (number(6, frame_id) if frame_id else b'') + binary(field, body))

            def connect(actor):
                if actor == 'agent':
                    return NativePeer(context, native_port, registered['credential'], purpose=2)
                return BrowserPeer(context, gateway_port, cookie=cookie, origin=origin)

            # A browser cannot spend the Agent's ticket, nor a ticket for the other purpose.
            for purpose, ticket in ((2, grants[2][3]), (3, browser_grants[2]['ticket'].encode())):
                with connect('browser') as rejected:
                    rejected.send(packet(2, 1, 16, number(1, purpose) + binary(2, ticket)))
                    rejected.rejected()

            with contextlib.ExitStack() as stack:
                channels = {}
                for purpose, actor in ((2, 'browser'), (2, 'agent'), (3, 'browser'), (3, 'agent')):
                    peer = stack.enter_context(connect(actor))
                    channels[purpose, actor] = peer
                    ticket = grants[purpose][3] if actor == 'agent' else browser_grants[purpose]['ticket'].encode()
                    peer.send(packet(purpose, 1, 16, number(1, purpose) + binary(2, ticket)))
                    if len(channels) == 3:
                        first = channels[2, 'browser']
                        first.socket.settimeout(.1)
                        try:
                            first.receive()
                            raise AssertionError('Desktop became ready without all four endpoints')
                        except socket.timeout:
                            pass
                        finally:
                            first.socket.settimeout(5)
                for (purpose, actor), peer in channels.items():
                    opcode, ready = peer.receive()
                    decoded = fields(ready)
                    assert opcode == 2 and decoded[4] == 1 and fields(decoded[29])[1] == purpose
                    assert decoded[2] == session['id'].encode() and decoded[3] == browser_grants[purpose]['id'].encode()
                assert request('GET', '/api/sessions/' + session['id'], cookie=cookie)[2]['state'] == 'active'
                with connect('browser') as replay:
                    replay.send(packet(2, 1, 16, number(1, 2) + binary(2, browser_grants[2]['ticket'].encode())))
                    replay.rejected()
                refresh = packet(2, 2, 25)
                channels[2, 'browser'].send(refresh)
                assert channels[2, 'agent'].receive() == (2, refresh)
                cursor = packet(2, 2, 23)
                channels[2, 'agent'].send(cursor)
                assert channels[2, 'browser'].receive() == (2, cursor)
                config = packet(3, 2, 18, binary(1, b'avc1.42C01E') + number(2, 16) + number(3, 16), generation=1)
                channels[3, 'agent'].send(config)
                assert channels[3, 'browser'].receive() == (2, config)
                # Larger than the old 64 KiB control limit; byte-for-byte relay across real TLS sockets.
                frame = packet(3, 3, 19, binary(1, b'v' * 70000) + number(2, 1), generation=1, frame_id=1)
                channels[3, 'agent'].send(frame)
                assert channels[3, 'browser'].receive() == (2, frame)
                if mode == 'disconnect':
                    # Keep both desktop WSS channels silent beyond the 45-second
                    # gateway idle timeout. Only RFC 6455 Ping/Pong may keep them alive.
                    for heartbeat in range(4):
                        control_sequence += 1
                        control.send(envelope(control_sequence, 12))
                        assert 12 in fields(control.receive()[1])
                        for purpose in (2, 3):
                            peer = channels[purpose, 'browser']
                            peer.socket.settimeout(20)
                            opcode, payload = peer.receive()
                            assert opcode == 9, 'Idle desktop channel did not send a WebSocket Ping'
                            peer.send(payload, 10)
                            peer.socket.settimeout(5)
                    control_sequence += 1
                    control.send(envelope(control_sequence, 12))
                    assert 12 in fields(control.receive()[1])
                    resumed = packet(2, 3, 25)
                    channels[2, 'browser'].send(resumed)
                    assert channels[2, 'agent'].receive() == (2, resumed)
                    assert request('GET', '/api/sessions/' + session['id'], cookie=cookie)[2]['state'] == 'active'
                    channels[3, 'agent'].socket.close()
                    survivors = [peer for key, peer in channels.items() if key != (3, 'agent')]
                elif mode == 'read_only':
                    channels[2, 'browser'].send(packet(2, 3, 22, number(1, 3) + number(4, 65), generation=1))
                    survivors = list(channels.values())
                else:
                    if mode == 'user_close':
                        assert request('POST', '/api/sessions/' + session['id'] + '/close', {}, cookie, origin)[0] == 200
                        assert 14 in fields(control.receive()[1])
                    else:
                        sql('UPDATE device_grants SET write_control=false')
                    survivors = list(channels.values())
                    for peer in survivors: peer.socket.settimeout(8)
                for peer in survivors:
                    peer.rejected()
                opcode, closed = control.receive()
                assert opcode == 2 and 14 in fields(closed)
                result = request('GET', '/api/sessions/' + session['id'], cookie=cookie)[2]
                assert result['state'] == 'closed'
                if mode == 'user_close': assert result['end_reason'] == 'user_closed'
                if mode == 'revoke':
                    assert result['end_reason'] == 'permission_revoked'
                    sql('UPDATE device_grants SET write_control=true')


def browser_pin(cert):
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
    public_key = x509.load_pem_x509_certificate(cert.read_bytes()).public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return base64.b64encode(hashlib.sha256(public_key).digest()).decode()


def synthetic_desktop(context, native_port, registered, cookie, origin, request, sql, executable, config, node, browser, cert, folder, login_password):
    # Keep production browser WSS, gateway routing and Agent native transport;
    # substitute only the DXGI-dependent worker and Windows input injection.
    observations = folder / 'synthetic-f24.txt'
    payload = json.dumps({'origin': origin, 'username': 'fixture-admin', 'password': login_password,
                          'pin': browser_pin(cert), 'observations': str(observations)}).encode()
    script = pathlib.Path(__file__).resolve().parents[1] / 'web' / 'desktop_synthetic.cjs'
    writable = {'device_id': registered['device_id'], 'kind': 'desktop', 'write_control': True, 'desktop_input_contract': 2}
    with connected_control(context, native_port, registered, number(2, 1) + number(3, 1)):
        status, _, denied = request('POST', '/api/sessions', writable, cookie, origin)
        assert status == 409 and 'desktop_input_upgrade_required' in json.dumps(denied), 'Old Agent accepted generation-bound desktop input'
        assert request('POST', '/api/sessions', dict(writable, device_id='d-not-owned'), cookie, origin)[0] == 403, 'Agent capabilities leaked across device authorization'
    with connected_control(context, native_port, registered, number(2, 1) + number(3, 1) + number(12, 1)):
        assert request('POST', '/api/sessions', writable, cookie, origin)[0] == 409, 'v1 Agent opened a v2 writable desktop'
    with connected_control(context, native_port, registered, number(2, 1) + number(3, 1) + number(12, 2)) as control:
        legacy = dict(writable)
        del legacy['desktop_input_contract']
        status, _, denied = request('POST', '/api/sessions', legacy, cookie, origin)
        assert status == 400 and 'desktop_input_upgrade_required' in json.dumps(denied), 'Old browser opened a writable desktop'
        assert request('POST', '/api/sessions', dict(writable, desktop_input_contract=1), cookie, origin)[0] == 400, 'v1 browser opened a v2 writable desktop'
        assert request('POST', '/api/sessions', dict(writable, desktop_input_contract=3), cookie, origin)[0] == 400, 'Unknown contract version was accepted'
        assert request('POST', '/api/sessions', dict(writable, write_control=False), cookie, origin)[0] == 400, 'Readonly session accepted a write contract'
        assert sql("SELECT count(*) FROM remote_sessions") == '0', 'Incompatible clients reserved or preempted a desktop session'
        print('Mixed-version clients rejected before desktop reservation; authorization checked before Agent capabilities', flush=True)
        control.socket.settimeout(25)
        browser_process = subprocess.Popen([str(node), str(script), str(browser)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        agent_process = None
        try:
            browser_process.stdin.write(payload)
            browser_process.stdin.close()
            browser_process.stdin = None
            opcode, command = control.receive()
            assert opcode == 2 and 13 in fields(command), 'Browser did not request a desktop session'
            agent_process = subprocess.Popen([str(executable), str(config), '--browser-fixture', str(observations)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
            agent_process.stdin.write(command)
            agent_process.stdin.close()
            agent_process.stdin = None
            browser_output, browser_error = browser_process.communicate(timeout=65)
            if browser_process.returncode != 0:
                peer_status = agent_process.poll()
                if peer_status is None: agent_process.terminate()
                peer_output, peer_error = agent_process.communicate(timeout=5)
                raise AssertionError(f'Browser: {browser_error.decode(errors="replace")}; native peer exit={peer_status}, stdout={peer_output.decode(errors="replace")}, stderr={peer_error.decode(errors="replace")}')
            agent_output, agent_error = agent_process.communicate(timeout=5)
            assert agent_process.returncode == 0, agent_error.decode(errors='replace')
            print(browser_output.decode(errors='replace').strip(), flush=True)
            print(agent_output.decode(errors='replace').strip(), flush=True)
        finally:
            for child in (browser_process, agent_process):
                if child is not None and child.poll() is None:
                    child.terminate()
                    child.wait(timeout=5)


def native_desktop(context, gateway_port, native_port, registered, cookie, origin, request, executable, config):
    with connected_control(context, native_port, registered, number(2, 1) + number(3, 1)) as control:
        status, _, session = request('POST', '/api/sessions',
            {'device_id': registered['device_id'], 'kind': 'desktop', 'write_control': False}, cookie, origin)
        assert status == 202
        opcode, command = control.receive()
        assert opcode == 2 and 13 in fields(command)
        process = subprocess.Popen([str(executable), str(config)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            process.stdin.write(command)
            process.stdin.close()
            process.stdin = None
            with contextlib.ExitStack() as stack:
                channels = {}
                grants = {grant['purpose']: grant for grant in session['channels']}
                for purpose, grant in grants.items():
                    peer = stack.enter_context(BrowserPeer(context, gateway_port, cookie=cookie, origin=origin))
                    channels[purpose] = peer
                    peer.send(envelope(1, 16, number(1, purpose) + binary(2, grant['ticket'].encode()), session=session['id'].encode()) + binary(3, grant['id'].encode()))
                for purpose, peer in channels.items():
                    opcode, ready = peer.receive()
                    assert opcode == 2 and fields(fields(ready)[29])[1] == purpose
                opcode, cursor = channels[2].receive()
                assert opcode == 2 and fields(cursor)[4] == 2 and 23 in fields(cursor)
                opcode, config_message = channels[3].receive()
                decoded = fields(config_message)
                assert opcode == 2 and decoded[4] == 2 and decoded[5] == 1
                video = fields(decoded[18])
                assert video[1].startswith(b'avc1.') and video[2] == video[3] == 32
                for index in (1, 2):
                    if index == 2:
                        channels[2].send(envelope(2, 25, session=session['id'].encode()) + binary(3, grants[2]['id'].encode()))
                    opcode, message = channels[3].receive()
                    decoded = fields(message)
                    frame = fields(decoded[19])
                    assert opcode == 2 and decoded[4] == index + 2 and decoded[5] == 1 and decoded[6] == index
                    assert frame[2] == 1 and frame[1].startswith((b'\0\0\0\1', b'\0\0\1'))
                    if index == 2: assert frame[3] == 33333
                channels[3].socket.close()
                stdout, stderr = process.communicate(timeout=5)
                assert process.returncode == 0, (stdout + stderr).decode(errors='replace')
                channels[2].rejected()
                assert 14 in fields(control.receive()[1])
                assert request('GET', '/api/sessions/' + session['id'], cookie=cookie)[2]['state'] == 'closed'
                print(stdout.decode().strip())
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)


def dispatched_desktop(context, gateway_port, registered, cookie, origin, request, executable, config, online, sql):
    process = subprocess.Popen([str(executable), '--connect', str(config)], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        deadline = time.monotonic() + 8
        while not online():
            assert process.poll() is None, 'Production Agent exited before connecting'
            assert time.monotonic() < deadline, 'Production Agent did not become online'
            time.sleep(.05)
        settings_route = '/api/devices/windows-login'
        settings_request = {'device_id': registered['device_id'], 'action': 'read'}
        assert request('POST', settings_route, settings_request, request_origin=origin)[0] == 401
        assert request('POST', settings_route, settings_request, cookie, 'https://attacker.invalid')[0] == 403
        for permission in ('desktop', 'write_control'):
            sql('UPDATE device_grants SET ' + permission + '=false')
            assert request('POST', settings_route, settings_request, cookie, origin)[0] == 403
            sql('UPDATE device_grants SET ' + permission + '=true')
        status, _, saved = request('POST', settings_route, settings_request, cookie, origin)
        assert status == 200 and saved == {'saved': False, 'username': '', 'domain': ''}
        password = secrets.token_hex(24)
        credentials = dict(settings_request, action='save', username='test-only-account', domain='', password=password)
        status, _, saved = request('POST', settings_route, credentials, cookie, origin)
        assert status == 200 and saved == {'saved': True, 'username': 'test-only-account', 'domain': ''}
        stored = config.parent / 'windows_login.credential'
        assert stored.exists() and password.encode() not in stored.read_bytes()
        assert password not in json.dumps(saved)
        credentials['username'] = 'updated-test-account'
        assert request('POST', settings_route, credentials, cookie, origin)[0] == 200
        status, _, saved = request('POST', settings_route, settings_request, cookie, origin)
        assert status == 200 and saved['username'] == 'updated-test-account'
        assert request('POST', settings_route, dict(settings_request, action='clear'), cookie, origin)[0] == 200
        assert not stored.exists()
        assert sql("SELECT count(*) FROM audit_events WHERE event_type='device.windows_login.save' AND success") == '2'
        assert sql("SELECT count(*) FROM audit_events WHERE event_type='device.windows_login.clear' AND success") == '1'
        print('Authorized Windows credential save/read/update/clear on Agent passed', flush=True)
        for writable in (False, True):
            status, _, session = request('POST', '/api/sessions',
                {'device_id': registered['device_id'], 'kind': 'desktop', 'write_control': writable,
                 **({'desktop_input_contract': 2} if writable else {})}, cookie, origin)
            assert status == 202, (status, session)
            grants = {grant['purpose']: grant for grant in session['channels']}
            def packet(purpose, sequence, field, body=b''):
                return envelope(sequence, field, body, session['id'].encode()) + binary(3, grants[purpose]['id'].encode())
            with contextlib.ExitStack() as stack:
                channels = {}
                for purpose, grant in grants.items():
                    peer = stack.enter_context(BrowserPeer(context, gateway_port, cookie=cookie, origin=origin))
                    channels[purpose] = peer
                    peer.send(packet(purpose, 1, 16, number(1, purpose) + binary(2, grant['ticket'].encode())))
                for purpose, peer in channels.items():
                    opcode, ready = peer.receive()
                    assert opcode == 2 and fields(fields(ready)[29])[1] == purpose
                opcode, data = channels[3].receive()
                configuration = fields(data)
                video = fields(configuration[18])
                assert opcode == 2 and video[1].startswith(b'avc1.') and video[2] > 32 and video[3] > 32
                opcode, data = channels[3].receive()
                first = fields(data)
                image = fields(first[19])
                assert opcode == 2 and image[2] == 1 and image[1].startswith((b'\0\0\0\1', b'\0\0\1'))
                input_sequence = 2
                channels[2].send(packet(2, input_sequence, 24, number(1, first[6]) + number(5, first[5])))
                input_sequence += 1
                channels[2].send(packet(2, input_sequence, 25))
                deadline = time.monotonic() + 5
                while True:
                    opcode, data = channels[3].receive()
                    refreshed = fields(data)
                    assert opcode == 2 and refreshed[5] == first[5] and refreshed[6] > first[6]
                    input_sequence += 1
                    channels[2].send(packet(2, input_sequence, 24, number(1, refreshed[6]) + number(5, refreshed[5])))
                    if fields(refreshed[19]).get(2) == 1: break
                    assert time.monotonic() < deadline, 'Agent did not honor the refresh request'
                if writable:
                    channels[2].send(packet(2, input_sequence + 1, 22, number(1, 5)) + number(5, first[5]))  # Release only this session's input.
                assert request('POST', '/api/sessions/' + session['id'] + '/close', {}, cookie, origin)[0] == 200
                for peer in channels.values():
                    deadline = time.monotonic() + 5
                    while True:
                        try:
                            opcode, data = peer.receive()
                            if opcode == 8:
                                peer.send(data, 8)
                                break
                        except (EOFError, ConnectionError):
                            break
                        assert time.monotonic() < deadline, 'Closed desktop channel kept sending data'
                result = request('GET', '/api/sessions/' + session['id'], cookie=cookie)[2]
                assert result['state'] == 'closed' and result['end_reason'] == 'user_closed', (writable, result)
                assert process.poll() is None and online(), 'Closing a desktop stopped the control connection'
        print('Production Agent control dispatch, real desktop frames, refresh and repeated session close passed')
    finally:
        if process.poll() is None:
            process.terminate()
        process.communicate(timeout=5)


def port(*, udp=False, excluded=()):
    # Windows can reserve different ranges for TCP and UDP. The native listener
    # requires both, and independent listeners must not reuse the same candidate.
    for _ in range(512):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            candidate = listener.getsockname()[1]
            if candidate in excluded:
                continue
            if udp:
                try:
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as datagram:
                        datagram.bind(('127.0.0.1', candidate))
                except OSError as error:
                    if error.errno not in (13, 48, 98, 10013, 10048):
                        raise
                    continue
            return candidate
    raise RuntimeError('No available loopback port for the required transports')


def execute(arguments, *, data=None, environment=None, expected=0, timeout=30):
    # pg_ctl launches a persistent child on Windows. Inherited PIPE handles can
    # keep communicate() blocked after pg_ctl itself exits; its server has a log file.
    launches_server = pathlib.Path(arguments[0]).name == 'pg_ctl.exe'
    result = subprocess.run([str(arg) for arg in arguments], input=data, stdout=subprocess.DEVNULL if launches_server else subprocess.PIPE,
                            stderr=subprocess.DEVNULL if launches_server else subprocess.PIPE, env=environment, timeout=timeout,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode != expected:
        raise AssertionError(f'{pathlib.Path(arguments[0]).name} failed ({result.returncode}): {(result.stderr or b"").decode(errors="replace")}\nTest progress: {(result.stdout or b"")[-4000:].decode(errors="replace")}')
    return result.stdout or b''


def unprotect(path):
    class Blob(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('data', ctypes.POINTER(ctypes.c_ubyte))]
    encrypted = path.read_bytes()
    raw = (ctypes.c_ubyte * len(encrypted)).from_buffer_copy(encrypted)
    source, output = Blob(len(encrypted), raw), Blob()
    crypt = ctypes.WinDLL('crypt32', use_last_error=True)
    crypt.CryptUnprotectData.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(Blob)]
    crypt.CryptUnprotectData.restype = wintypes.BOOL
    if not crypt.CryptUnprotectData(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(output)):
        raise OSError(ctypes.get_last_error(), 'Cannot decrypt enrolled credential')
    try:
        return ctypes.string_at(output.data, output.size).decode()
    finally:
        ctypes.memset(raw, 0, len(raw))
        ctypes.memset(output.data, 0, output.size)
        kernel = ctypes.WinDLL('kernel32')
        kernel.LocalFree.argtypes = [ctypes.c_void_p]
        kernel.LocalFree(output.data)


def run(gateway, agent, postgres, workspace, session_test, desktop_test, native_test, node=None, browser=None, web_root=None, input_observer=None, synthetic=False):
    motion_seconds = int(os.environ.get('ULTRATERIMAL_DESKTOP_MOTION_SECONDS', '3'))
    if not 3 <= motion_seconds <= 3600:
        raise ValueError('Desktop motion duration must be between 3 and 3600 seconds')
    faulthandler.dump_traceback_later(180 + motion_seconds)
    workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gateway-', dir=workspace) as temporary:
        folder = pathlib.Path(temporary).resolve()
        assert folder.is_relative_to(workspace.resolve())
        cluster = folder / 'postgres'
        password = secrets.token_hex(24)
        password_file = folder / 'postgres-password'
        password_file.write_text(password, encoding='ascii')
        db_port = port()
        gateway_port = port(excluded={db_port})
        native_port = port(udp=True, excluded={db_port, gateway_port})
        execute([postgres / 'initdb.exe', '-D', cluster, '-U', 'ultraterimal_test', '--pwfile', password_file,
                 '--auth=scram-sha-256', '--encoding=UTF8', '--no-locale'])
        password_file.unlink()
        environment = dict(os.environ, PGPASSWORD=password, PGCONNECT_TIMEOUT='3')
        running_database = False
        process = None
        try:
            running_database = True
            execute([postgres / 'pg_ctl.exe', '-D', cluster, '-l', folder / 'postgres.log', '-o',
                     f'-h 127.0.0.1 -p {db_port}', '-w', '-t', '20', 'start'])
            running_database = True
            cert, key = certificate(folder)
            origin = f'https://localhost:{gateway_port}'
            settings = folder / 'gateway.json'
            settings.write_text(json.dumps({'address':'127.0.0.1','port':gateway_port,'certificate_file':str(cert),
                                            'key_file':str(key),'origin':origin,
                                            'native_address':'127.0.0.1','native_public_address':'127.0.0.1','native_port':native_port,
                                            'native_certificate_file':str(cert),'native_key_file':str(key),
                                            'database':f'host=127.0.0.1 port={db_port} dbname=postgres user=ultraterimal_test password={password}'}), encoding='utf8')
            fixture_web = pathlib.Path(web_root) if web_root else folder / 'web'
            if not web_root:
                fixture_web.mkdir()
                (fixture_web / 'install.ps1').write_text("Write-Output 'installer fixture'\n", encoding='utf8')
            configuration = json.loads(settings.read_text(encoding='utf8'))
            configuration['web_root'] = str(fixture_web)
            settings.write_text(json.dumps(configuration), encoding='utf8')
            execute([gateway, '--migrate', settings])
            execute([gateway, '--migrate', settings])  # Migration replay must be harmless.
            login_password = secrets.token_hex(16)
            bootstrap = json.dumps({'username':'fixture-admin','password':login_password}).encode()
            execute([gateway, '--bootstrap', settings], data=bootstrap)
            execute([gateway, '--bootstrap', settings], data=bootstrap, expected=1)
            log = (folder / 'gateway.log').open('wb')
            process = subprocess.Popen([str(gateway), '--serve', str(settings)], stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            context = ssl.create_default_context(cafile=str(cert))
            context.set_alpn_protocols(['ultraterimal/1', 'http/1.1'])

            def request(method, route, body=None, cookie=None, request_origin=None, headers=None):
                connection = http.client.HTTPSConnection('localhost', gateway_port, context=context, timeout=5)
                connection.sock = context.wrap_socket(socket.create_connection(('127.0.0.1', gateway_port), timeout=5), server_hostname='localhost')
                request_headers = dict(headers or {})
                if body is not None: request_headers['Content-Type'] = 'application/json'
                if cookie: request_headers['Cookie'] = cookie
                if request_origin: request_headers['Origin'] = request_origin
                try:
                    connection.request(method, route, json.dumps(body).encode() if body is not None else None, request_headers)
                    response = connection.getresponse()
                    content = response.read()
                    parsed = (json.loads(content) if response.getheader('Content-Type', '').startswith('application/json') else content.decode('utf8')) if content else None
                    return response.status, dict(response.getheaders()), parsed
                finally:
                    connection.close()

            for _ in range(50):
                if process.poll() is not None: raise AssertionError('Gateway exited during startup')
                try:
                    if request('GET','/api/health')[0] == 200: break
                except (OSError, http.client.HTTPException): pass
                time.sleep(.1)
            else: raise AssertionError('Gateway did not become ready')
            if not web_root:
                assert request('GET', '/install.ps1')[2].replace('\r\n', '\n') == "Write-Output 'installer fixture'\n", 'PowerShell static script must be served'
            print(execute([native_test, cert, str(native_port), 'gateway']).decode().strip(), flush=True)
            untrusted = folder / 'untrusted'
            untrusted.mkdir()
            wrong_ca, _ = certificate(untrusted)
            print(execute([native_test, wrong_ca, str(native_port), 'untrusted']).decode().strip(), flush=True)
            assert request('GET','/api/devices')[0] == 401
            assert request('POST','/api/enrollments',{},request_origin=origin)[0] == 401
            login = {'username':'fixture-admin','password':login_password}
            assert request('POST','/api/login',login,request_origin='https://attacker.invalid')[0] == 403
            assert request('POST','/api/login',{'username':"' OR TRUE --",'password':login_password},request_origin=origin)[0] == 401
            status, headers, user = request('POST','/api/login',login,request_origin=origin)
            assert status == 200, f'Login returned {status}'
            cookie_header = next(value for name, value in headers.items() if name.lower() == 'set-cookie')
            assert all(flag in cookie_header for flag in ('Secure','HttpOnly','SameSite=Strict','Path=/'))
            cookie = cookie_header.split(';')[0]
            assert request('GET','/api/me',cookie=cookie)[2]['id'] == user['id']
            status, _, pending = request('POST','/api/devices/create',{'name':'本机 Windows Agent'},cookie,origin)
            assert status == 201 and pending['enrolled'] is False
            assert any(d['id'] == pending['id'] for d in request('GET','/api/devices',cookie=cookie)[2])
            enrollment_body = {'device_id':pending['id'],'platform':'windows-x64'}
            assert request('POST','/api/enrollments',dict(enrollment_body, platform='linux'),cookie,origin)[0] == 400
            assert request('POST','/api/enrollments',dict(enrollment_body, device_id='d-not-owned'),cookie,origin)[0] == 403
            status, _, enrollment = request('POST','/api/enrollments',enrollment_body,cookie,origin)
            assert enrollment['device_id'] == pending['id']
            assert enrollment['web_origin'] == origin
            assert enrollment['native_host'] == 'localhost'
            assert enrollment['native_address'] == '127.0.0.1'
            assert enrollment['native_port'] == native_port
            assert status == 201
            install_status, install_headers, install_script = request('GET', '/api/install.ps1?code=' + enrollment['code'])
            assert install_status == 200 and pending['id'] in install_script and enrollment['code'] in install_script
            assert any(k.lower() == 'cache-control' and v == 'no-store' for k, v in install_headers.items())
            assert request('GET', '/api/install.ps1?code=invalid')[0] == 404
            assert request('GET', '/api/install.ps1?device=d-invalid')[0] == 404
            registration = {'code':enrollment['code'],'name':'本机 Windows Agent'}
            assert request('POST','/api/agent/register',request_origin=origin)[0] == 404
            protected = folder / 'agent.credential'
            bootstrap_config = folder / 'enrollment.json'
            bootstrap_config.write_text(json.dumps({'host':'localhost','address':'127.0.0.1','port':str(native_port),'ca_file':str(cert),
                                                    'credential_file':str(protected)}), encoding='utf8')
            registration_input = json.dumps(registration).encode()
            generated = execute([agent, '--enroll', bootstrap_config], data=registration_input)
            print('Native enrollment completed', flush=True)
            config = json.loads(generated)
            registered = {'device_id':config['device_id'], 'credential':unprotect(protected)}
            assert registered['device_id'] == pending['id'], 'Enrollment created a duplicate device'
            assert request('POST','/api/enrollments',enrollment_body,cookie,origin)[2]['code'] == '', 'Enrolled identity must not be replaced'
            assert request('GET', '/api/install.ps1?code=' + enrollment['code'])[0] == 404
            repair_status, _, repair_script = request('GET', '/api/install.ps1?device=' + pending['id'])
            assert repair_status == 200 and "$utCode=''" in repair_script
            assert registered['credential'].encode() not in generated and enrollment['code'].encode() not in generated
            assert registered['credential'].encode() not in protected.read_bytes()
            previous = protected.read_bytes()
            execute([agent, '--enroll', bootstrap_config], data=registration_input, expected=1)
            assert protected.read_bytes() == previous, 'Enrollment overwrote an existing identity'
            consumed_config = dict(json.loads(bootstrap_config.read_text(encoding='utf8')), credential_file=str(folder / 'consumed.credential'))
            bootstrap_config.write_text(json.dumps(consumed_config), encoding='utf8')
            execute([agent, '--enroll', bootstrap_config], data=registration_input, expected=1)
            assert not (folder / 'consumed.credential').exists(), 'Consumed enrollment code issued another identity'
            status, _, listed = request('GET','/api/devices',cookie=cookie)
            assert status == 200 and len(listed) == 1 and listed[0]['id'] == registered['device_id'] and not listed[0]['online']

            assert listed[0]['can_manage'] is True
            rename_body = {'device_id': registered['device_id'], 'name': 'Renamed fixture'}
            assert request('POST', '/api/devices/rename', rename_body, cookie, 'https://attacker.invalid')[0] == 403
            assert request('POST', '/api/devices/rename', rename_body, request_origin=origin)[0] == 401
            assert request('POST', '/api/devices/rename', dict(rename_body, device_id='d-not-owned'), cookie, origin)[0] == 403
            assert request('POST', '/api/devices/rename', rename_body, cookie, origin)[0] == 200
            assert request('GET', '/api/devices', cookie=cookie)[2][0]['name'] == 'Renamed fixture'

            def sql(query):
                return execute([postgres / 'psql.exe','-h','127.0.0.1','-p',str(db_port),'-U','ultraterimal_test','-d','postgres','-tA','-c',query],environment=environment).decode().strip()

            assert sql('SELECT credential_hash FROM devices') == hashlib.sha256(registered['credential'].encode()).hexdigest()
            agent_config = folder / 'agent.json'
            agent_config.write_bytes(generated)
            execute([agent, '--connection-check', agent_config])
            print('Native control handshake completed', flush=True)
            assert sql('SELECT count(*) FROM device_capabilities') == '1'
            assert sql('SELECT count(*) FROM devices WHERE last_seen_at IS NOT NULL') == '1'
            assert sql("SELECT count(*) FROM audit_events WHERE event_type='device.register' AND success") == '1'

            def online():
                status, _, devices = request('GET','/api/devices',cookie=cookie)
                assert status == 200
                return any(device['id'] == registered['device_id'] and device['online'] for device in devices)

            def wait_offline(timeout=5):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if not online(): return
                    time.sleep(.5 if timeout > 5 else .05)
                raise AssertionError('Disconnected device remained online')

            wait_offline()
            if synthetic:
                assert node and browser and web_root, 'Synthetic desktop requires the built frontend and a browser'
                synthetic_desktop(context, native_port, registered, cookie, origin, request, sql, desktop_test, agent_config, node, browser, cert, folder, login_password)
                print(execute([session_test], data=(json.loads(settings.read_text(encoding='utf8'))['database'] + '\n').encode()).decode().strip(), flush=True)
                return
            # Exercise immediate reuse: a stale control binding must not survive
            # a successfully completed connection check or block the next one.
            for _ in range(4):
                execute([agent, '--connection-check', agent_config])
                wait_offline()
            restart_request = {'deviceId':registered['device_id'],'idempotencyKey':'restart-fixture-key'}
            sql("UPDATE device_grants SET write_control=false WHERE device_id='" + registered['device_id'] + "'")
            assert request('POST','/api/devices/restart',restart_request,cookie,origin)[0] == 403, 'Restart requires control permission'
            sql("UPDATE device_grants SET write_control=true WHERE device_id='" + registered['device_id'] + "'")
            assert request('POST','/api/devices/restart',dict(restart_request, deviceId='d-not-owned'),cookie,origin)[0] == 403, 'Unowned devices must not be enumerable'
            assert request('POST','/api/devices/restart',restart_request,cookie,'https://attacker.invalid')[0] == 403
            assert request('POST','/api/devices/restart',restart_request,request_origin=origin)[0] == 401
            assert request('POST','/api/devices/restart',restart_request,cookie,origin)[0] == 409, 'Offline restart should be rejected without a separate permission step'
            with connected_control(context, native_port, registered) as fake_agent:
                assert request('POST','/api/devices/restart',restart_request,cookie,origin)[0] == 409, 'Unsupported restart capability must be rejected'
            wait_offline()
            restart_capabilities = number(10, 1) + number(11, 10)
            with connected_control(context, native_port, registered, restart_capabilities) as fake_agent:
                status, _, restart_operation = request('POST','/api/devices/restart',restart_request,cookie,origin)
                assert status == 202 and restart_operation['status'] == 'requested'
                opcode, command = fake_agent.receive()
                command_fields = fields(command)
                assert opcode == 2 and 37 in command_fields, 'Gateway did not dispatch the native restart request'
                operation = fields(command_fields[37])
                assert operation[2] == registered['device_id'].encode()
                assert restart_operation['id'] == operation[1].decode()
                status, _, pending = request('GET','/api/devices/restart/' + operation[1].decode(),cookie=cookie)
                assert status == 200 and pending['status'] == 'requested'
                duplicate_status, _, duplicate = request('POST','/api/devices/restart',restart_request,cookie,origin)
                assert duplicate_status == 202 and duplicate['id'] == operation[1].decode()
                fake_agent.send(envelope(2, 38, binary(1, operation[1]) + number(2, 1)))
                fake_agent.send(envelope(2, 39, binary(1, operation[1]) + number(2, 1)))
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    status, _, current = request('GET','/api/devices/restart/' + restart_operation['id'],cookie=cookie)
                    if current['status'] == 'agent_accepted': break
                    time.sleep(.05)
                assert status == 200 and current['status'] == 'agent_accepted'
                assert sql("SELECT count(*) FROM audit_events WHERE event_type='device.system_restart.request' AND success") == '1'
                assert sql("SELECT count(*) FROM audit_events WHERE event_type='device.system_restart.result' AND success AND reason='ACCEPTED'") == '1'
            wait_offline()
            # A fresh explicit click is independent of historical outcomes.
            with connected_control(context, native_port, registered, restart_capabilities) as fake_agent:
                next_request = {'deviceId':registered['device_id'],'idempotencyKey':'next-explicit-click'}
                status, _, next_operation = request('POST','/api/devices/restart',next_request,cookie,origin)
                assert status == 202 and next_operation['id'] != restart_operation['id'], 'Prior accepted request must not require a manual workflow'
                opcode, command = fake_agent.receive()
                assert opcode == 2 and fields(fields(command)[37])[1].decode() == next_operation['id']
                fake_agent.send(envelope(2, 38, binary(1, next_operation['id'].encode()) + number(2, 2) + number(3, 5)))
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    status, _, fresh = request('GET', '/api/devices/restart/' + next_operation['id'], cookie=cookie)
                    if fresh['status'] == 'failed': break
                    time.sleep(.05)
                assert status == 200 and fresh['status'] == 'failed'
            wait_offline()
            retry_status, _, retry = request('POST','/api/devices/restart',restart_request,cookie,origin)
            assert retry_status == 202 and retry['id'] == restart_operation['id'], 'Idempotent retry must survive device disconnect'
            with connected_control(context, native_port, registered, number(10, 1) + number(11, 20)):
                status, _, current = request('GET','/api/devices/restart/' + restart_operation['id'],cookie=cookie)
                assert status == 200 and current['status'] == 'agent_accepted', 'Clock adjustment must not confirm a Windows restart'
            wait_offline()
            with connected_control(context, native_port, registered, number(10, 1) + number(11, 10)):
                status, _, current = request('GET','/api/devices/restart/' + restart_operation['id'],cookie=cookie)
                assert status == 200 and current['status'] == 'agent_accepted', 'An Agent-only reconnect must not confirm a Windows restart'
            wait_offline()
            assert sql("SELECT count(*) FROM audit_events WHERE event_type='device.system_restart.confirmed'") == '0'
            session_request = {'device_id':registered['device_id'],'kind':'desktop','write_control':True,'preempt':True,'desktop_input_contract':2}
            assert request('POST','/api/sessions',dict(session_request, kind='ssh', operating_system_user='Administrator'),cookie,origin)[0] == 400
            assert sql("SELECT count(*) FROM information_schema.columns WHERE table_name='remote_sessions' AND column_name='operating_system_user'") == '0'
            assert request('POST','/api/sessions',session_request,cookie,origin)[0] == 409
            with NativePeer(context, native_port, registered['credential']) as peer:
                assert not online(), 'An unfinished HELLO must not advertise online'
                peer.hello(registered['device_id'])
                assert online()
                assert request('POST','/api/sessions',session_request,cookie,origin)[0] == 409
                with NativePeer(context, native_port, registered['credential']) as duplicate:
                    duplicate.rejected()
                assert online(), 'Duplicate rejection removed the active device'
                peer.send(envelope(2, 12))
                opcode, data = peer.receive()
                assert opcode == 2 and fields(data)[4] == 2 and 12 in fields(data)
                peer.send(envelope(2, 12))  # Replayed sequence must terminate this connection.
                peer.rejected()
            wait_offline()
            with NativePeer(context, native_port, registered['credential']) as peer:
                peer.send(envelope(1, 10, binary(1, b'wrong-device') + binary(2, b'fixture')))
                peer.rejected()
            assert not online()
            session_secrets = []
            with connected_control(context, native_port, registered, number(2, 1) + number(3, 1) + number(12, 2)) as peer:
                status, _, session = request('POST','/api/sessions',session_request,cookie,origin)
                assert status == 202 and session['state'] == 'pending' and len(session['channels']) == 2
                opcode, command_bytes = peer.receive()
                command = fields(command_bytes)
                assert opcode == 2 and command[2].decode() == session['id'] and command[4] == 2
                opened = fields(command[13])
                assert opened[1] == 1 and opened[2].decode() == user['id'] and opened[5] == 1
                agent_grant = fields(opened[6])
                agent_ticket = agent_grant[3].decode()
                session_secrets.append(agent_ticket)
                for channel in session['channels']:
                    session_secrets.append(channel['ticket'])
                    assert channel['id'].encode() in command_bytes and channel['ticket'].encode() not in command_bytes
                assert agent_ticket not in json.dumps(session)
                status, _, replacement_session = request('POST','/api/sessions',session_request,cookie,origin)
                assert status == 202
                opcode, preempted_command = peer.receive()
                assert opcode == 2 and fields(preempted_command)[4] == 3 and fields(preempted_command)[2].decode() == session['id'] and 14 in fields(preempted_command)
                opcode, replacement_command = peer.receive()
                assert opcode == 2 and fields(replacement_command)[4] == 4 and fields(replacement_command)[2].decode() == replacement_session['id'] and 13 in fields(replacement_command)
                preempted = request('GET','/api/sessions/' + session['id'],cookie=cookie)[2]
                assert preempted['state'] == 'closed' and preempted['end_reason'] == 'preempted'
                peer.send(envelope(2, 15, binary(1, b'capability_unavailable') + binary(2, b'fixture declined'), session=replacement_session['id'].encode()))
                peer.send(envelope(3, 12))
                opcode, reply = peer.receive()
                assert opcode == 2 and fields(reply)[4] == 5 and 12 in fields(reply)
                status, _, closed = request('GET','/api/sessions/' + replacement_session['id'],cookie=cookie)
                assert status == 200 and closed['state'] == 'closed' and closed['end_reason'] == 'session_failed'
                assert request('GET','/api/sessions/' + replacement_session['id'])[0] == 401
                status, _, next_session = request('POST','/api/sessions',session_request,cookie,origin)
                assert status == 202
                opcode, next_command = peer.receive()
                assert opcode == 2 and fields(next_command)[4] == 6
                assert request('POST','/api/sessions/' + next_session['id'] + '/close',{},cookie,origin)[0] == 200
                opcode, closing = peer.receive()
                assert opcode == 2 and fields(closing)[4] == 7 and 14 in fields(closing)
                status, _, completed_session = request('POST','/api/sessions',session_request,cookie,origin)
                assert status == 202
                opcode, completed_open = peer.receive()
                assert opcode == 2 and fields(completed_open)[4] == 8 and 13 in fields(completed_open)
                peer.send(envelope(4, 14, binary(1, b'completed'), session=completed_session['id'].encode()))
                peer.send(envelope(5, 12))
                opcode, acknowledgment = peer.receive()
                assert opcode == 2 and fields(acknowledgment)[4] == 9 and 12 in fields(acknowledgment)
                completed = request('GET','/api/sessions/' + completed_session['id'],cookie=cookie)[2]
                assert completed['state'] == 'closed' and completed['end_reason'] == 'agent_closed'
                assert sql("SELECT count(*) FROM audit_events WHERE reason='agent_closed' AND success") == '1'
                status, _, disconnect_session = request('POST','/api/sessions',session_request,cookie,origin)
                assert status == 202
                peer.receive()
            wait_offline()
            for _ in range(50):
                closed = request('GET','/api/sessions/' + disconnect_session['id'],cookie=cookie)[2]
                if closed['state'] == 'closed': break
                time.sleep(.02)
            assert closed['state'] == 'closed' and closed['end_reason'] == 'agent_disconnected'
            if not node:
                desktop_channels(context, gateway_port, native_port, registered, cookie, origin, request, session_secrets, sql)
                print('Native TCP/browser relay and revocation completed', flush=True)
                wait_offline()
                native_desktop(context, gateway_port, native_port, registered, cookie, origin, request, desktop_test, agent_config)
                wait_offline()
                dispatched_desktop(context, gateway_port, registered, cookie, origin, request, agent, agent_config, online, sql)
                # TerminateProcess cannot emit a QUIC close; validate the control watchdog.
                wait_offline(timeout=40)
            if node:
                pin = browser_pin(cert)
                browser_agent = subprocess.Popen([str(agent), '--connect', str(agent_config)], stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    deadline = time.monotonic() + 8
                    while not online():
                        assert browser_agent.poll() is None and time.monotonic() < deadline
                        time.sleep(.05)
                    script = pathlib.Path(__file__).resolve().parents[1] / 'web' / 'desktop.cjs'
                    payload = json.dumps({'origin': origin, 'username': 'fixture-admin', 'password': login_password,
                        'pin': pin, 'screenshots': str(workspace / 'browser-ui'), 'inputObserver': str(input_observer),
                        'motionSeconds': motion_seconds}).encode()
                    print(execute([node, script, browser], data=payload, timeout=150 + motion_seconds).decode().strip())
                    deadline = time.monotonic() + 5
                    children = agent_children(browser_agent.pid)
                    while children and time.monotonic() < deadline:
                        time.sleep(.05)
                        children = agent_children(browser_agent.pid)
                    assert not children, f'Desktop cycles left child processes: {children}'
                    assert browser_agent.poll() is None and online(), 'Desktop cycles stopped the Agent control connection'
                    print('Desktop cycles released every desktop child while preserving the Agent control connection', flush=True)
                finally:
                    if browser_agent.poll() is None: browser_agent.terminate()
                    browser_agent.wait(timeout=5)
                wait_offline(timeout=40)
            with connected_control(context, native_port, registered) as peer:
                assert online()
                delete_body = {'device_id': registered['device_id']}
                assert request('POST', '/api/devices/delete', delete_body, cookie, 'https://attacker.invalid')[0] == 403
                assert request('POST', '/api/devices/delete', delete_body, request_origin=origin)[0] == 401
                assert request('POST', '/api/devices/delete', dict(delete_body, device_id='d-not-owned'), cookie, origin)[0] == 403
                assert request('POST', '/api/devices/delete', delete_body, cookie, origin)[0] == 200
                assert request('GET', '/api/devices', cookie=cookie)[2] == []
                peer.rejected()
            wait_offline()
            # Revocation must also reject a new connection at the native authentication boundary.
            execute([agent, '--connection-check', agent_config],expected=1)
            assert request('POST','/api/logout',{},cookie,origin)[0] == 200
            assert request('GET','/api/me',cookie=cookie)[0] == 401
            print(execute([session_test], data=(json.loads(settings.read_text(encoding='utf8'))['database'] + '\n').encode()).decode().strip())
            unfinished = sql("SELECT count(*) FROM remote_sessions WHERE state<>'closed'")
            assert int(unfinished) > 0
            competing = folder / 'competing-gateway.json'
            competing_config = json.loads(settings.read_text(encoding='utf8'))
            competing_config['port'] = port()
            competing_config['native_port'] = port()
            competing.write_text(json.dumps(competing_config), encoding='utf8')
            execute([gateway, '--serve', competing], expected=1)
            assert sql("SELECT count(*) FROM remote_sessions WHERE state<>'closed'") == unfinished
            process.terminate()
            process.wait(timeout=10)
            process = subprocess.Popen([str(gateway), '--serve', str(settings)], stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            for _ in range(50):
                if process.poll() is not None: raise AssertionError('Gateway restart failed')
                try:
                    if request('GET','/api/health')[0] == 200: break
                except (OSError, http.client.HTTPException): pass
                time.sleep(.1)
            else: raise AssertionError('Gateway restart did not become ready')
            assert sql("SELECT count(*) FROM remote_sessions WHERE state<>'closed'") == '0'
            assert sql("SELECT count(*) FROM audit_events WHERE event_type='session.end' AND reason='gateway_restarted'") == unfinished
            assert sql("SELECT pg_terminate_backend(pid) FROM pg_locks WHERE locktype='advisory' AND classid=0 AND objid=842105213 AND granted") == 't'
            assert process.wait(timeout=10) == 1, 'Lost database lease did not stop the gateway'
            log.close()
            logs = (folder / 'gateway.log').read_text(encoding='utf8',errors='replace')
            assert all(secret not in logs for secret in (login_password,password,enrollment['code'],registered['credential'],*session_secrets))
            print('Verified Ruvia TLS, PostgreSQL migrations, Agent registration and heartbeat, control authorization, session tickets, restart recovery and database lease loss')
        except Exception:
            if process is not None:
                print('Gateway process exit status:', process.poll(), flush=True)
                if 'log' in locals(): log.flush()
                diagnostic = (folder / 'gateway.log').read_text(encoding='utf8', errors='replace')
                sensitive = [password, locals().get('login_password', '')]
                sensitive += [locals().get('enrollment', {}).get('code', ''), locals().get('registered', {}).get('credential', '')]
                for secret in sensitive:
                    if secret: diagnostic = diagnostic.replace(secret, '[redacted]')
                print(diagnostic[-4000:], flush=True)
            raise
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            if 'log' in locals(): log.close()
            if running_database and (cluster / 'postmaster.pid').exists():
                execute([postgres / 'pg_ctl.exe','-D',cluster,'-m','fast','-w','-t','20','stop'])


if __name__ == '__main__':
    synthetic = sys.argv[-1] == '--synthetic'
    arguments = sys.argv[1:-1] if synthetic else sys.argv[1:]
    run(*(pathlib.Path(argument).resolve() for argument in arguments), synthetic=synthetic)
    faulthandler.cancel_dump_traceback_later()
