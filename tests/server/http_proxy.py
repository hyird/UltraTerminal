"""Loopback TLS termination for HTTP-only gateway integration fixtures."""
import asyncio
import ssl
import threading


class HttpProxy:
    def __init__(self, listen_port, upstream_port, certificate, key):
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.tasks = set()
        self.upstream_port = upstream_port
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)

        async def start():
            return await asyncio.start_server(self.forward, '127.0.0.1', listen_port, ssl=context)

        try:
            self.server = asyncio.run_coroutine_threadsafe(start(), self.loop).result(timeout=10)
        except BaseException:
            self.loop.call_soon_threadsafe(self.loop.stop)
            self.thread.join(timeout=10)
            self.loop.close()
            raise

    async def forward(self, reader, writer):
        current = asyncio.current_task()
        self.tasks.add(current)
        upstream = None
        pumps = []

        async def copy(source, destination):
            while data := await source.read(65536):
                destination.write(data)
                await destination.drain()

        try:
            source, upstream = await asyncio.open_connection('127.0.0.1', self.upstream_port)
            pumps = [asyncio.create_task(copy(reader, upstream)), asyncio.create_task(copy(source, writer))]
            await asyncio.wait(pumps, return_when=asyncio.FIRST_COMPLETED)
        except (ConnectionError, OSError):
            pass
        finally:
            for pump in pumps:
                pump.cancel()
            await asyncio.gather(*pumps, return_exceptions=True)
            for stream in (writer, upstream):
                if stream:
                    stream.close()
                    try:
                        await asyncio.wait_for(stream.wait_closed(), timeout=2)
                    except (ConnectionError, OSError, TimeoutError):
                        pass
            self.tasks.discard(current)

    def close(self):
        async def stop():
            self.server.close()
            pending = list(self.tasks)
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            await self.server.wait_closed()

        try:
            asyncio.run_coroutine_threadsafe(stop(), self.loop).result(timeout=10)
        finally:
            self.loop.call_soon_threadsafe(self.loop.stop)
            self.thread.join(timeout=10)
            self.loop.close()
