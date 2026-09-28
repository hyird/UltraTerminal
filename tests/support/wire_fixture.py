"""Independent protobuf and native record encoding for integration peers."""
import struct

def varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def number(field, value):
    return varint(field << 3) + varint(value)


def binary(field, value):
    return varint((field << 3) | 2) + varint(len(value)) + value


def envelope(sequence, field, body=b"", session=b""):
    return number(1, 1) + (binary(2, session) if session else b"") + number(4, sequence) + binary(field, body)


def fields(data, repeated=False):
    offset = 0
    result = {}

    def integer():
        nonlocal offset
        value = 0
        for shift in range(0, 70, 7):
            item = data[offset]
            offset += 1
            value |= (item & 127) << shift
            if not item & 128:
                return value
        raise AssertionError("Invalid protobuf integer")

    while offset < len(data):
        tag = integer()
        if tag & 7 == 0:
            value = integer()
        elif tag & 7 == 2:
            size = integer()
            assert size <= len(data) - offset
            value = data[offset:offset + size]
            offset += size
        else:
            raise AssertionError("Unexpected fixture wire type")
        if repeated:
            result.setdefault(tag >> 3, []).append(value)
        else:
            result[tag >> 3] = value
    return result


def record(data):
    return struct.pack('!I', len(data)) + data


async def read_record(reader):
    size, = struct.unpack('!I', await reader.readexactly(4))
    assert 0 < size <= 16 * 1024 * 1024
    return await reader.readexactly(size)


async def authenticate(reader, writer, purpose=None):
    request = fields(await read_record(reader))
    assert request[1] == 1 and request[3] == b'a' * 64
    assert request[2] in (1, 2) and (purpose is None or request[2] == purpose)
    writer.write(record(number(1, 1) + number(2, 1) + binary(3, b'fixture-device')))
    await writer.drain()
    return request[2]
