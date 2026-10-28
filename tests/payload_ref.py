"""Reference reader and writer for A/B OTA payloads (payload.bin), for the tests.

Independent of the C code: only the protobuf wire format and the fields of
update_metadata.proto that a full OTA uses.
"""
import hashlib
import struct

MAGIC = b"CrAU"

# InstallOperation.Type
REPLACE, REPLACE_BZ, ZERO, DISCARD, REPLACE_XZ, REPLACE_ZSTD = 0, 1, 6, 7, 8, 14


# ---- protobuf wire format ----

def _varint(buf, pos):
    v = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        if b < 0x80:
            return v, pos
        shift += 7


def fields(buf):
    """Yields (field_number, wire_type, value) for one message."""
    pos = 0
    while pos < len(buf):
        key, pos = _varint(buf, pos)
        num, wt = key >> 3, key & 7
        if wt == 0:
            v, pos = _varint(buf, pos)
        elif wt == 1:
            v = struct.unpack_from("<Q", buf, pos)[0]
            pos += 8
        elif wt == 2:
            n, pos = _varint(buf, pos)
            v = bytes(buf[pos:pos + n])
            pos += n
        elif wt == 5:
            v = struct.unpack_from("<I", buf, pos)[0]
            pos += 4
        else:
            raise ValueError(f"wire type {wt}")
        yield num, wt, v


def enc_varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def enc_field(num, value):
    if isinstance(value, int):
        return enc_varint(num << 3) + enc_varint(value)
    return enc_varint(num << 3 | 2) + enc_varint(len(value)) + value


# ---- reading ----

def parse_extent(buf):
    start = count = 0
    for n, _, v in fields(buf):
        if n == 1:
            start = v
        elif n == 2:
            count = v
    return start, count


def parse_operation(buf):
    op = {"type": 0, "data_offset": 0, "data_length": 0, "dst": [], "hash": b""}
    for n, _, v in fields(buf):
        if n == 1:
            op["type"] = v
        elif n == 2:
            op["data_offset"] = v
        elif n == 3:
            op["data_length"] = v
        elif n == 6:
            op["dst"].append(parse_extent(v))
        elif n == 8:
            op["hash"] = v
    return op


def parse_partition(buf):
    p = {"name": "", "size": 0, "hash": b"", "ops": []}
    for n, _, v in fields(buf):
        if n == 1:
            p["name"] = v.decode()
        elif n == 7:
            for m, _, w in fields(v):
                if m == 1:
                    p["size"] = w
                elif m == 2:
                    p["hash"] = w
        elif n == 8:
            p["ops"].append(parse_operation(v))
    return p


def parse_groups(buf):
    groups = []
    for n, _, v in fields(buf):
        if n == 1:
            g = {"name": "", "size": 0, "partitions": []}
            for m, _, w in fields(v):
                if m == 1:
                    g["name"] = w.decode()
                elif m == 2:
                    g["size"] = w
                elif m == 3:
                    g["partitions"].append(w.decode())
            groups.append(g)
    return groups


def read_payload(f, base=0):
    """f: binary file object; base: payload offset inside it (e.g. in a zip)."""
    f.seek(base)
    head = f.read(24)
    if head[:4] != MAGIC:
        raise ValueError("not a payload")
    version, manifest_size, sig_size = struct.unpack(">QQI", head[4:24])
    manifest = f.read(manifest_size)
    m = {"version": version, "block_size": 4096, "partitions": [], "groups": [],
         "data_offset": base + 24 + manifest_size + sig_size}
    for n, _, v in fields(manifest):
        if n == 3:
            m["block_size"] = v
        elif n == 13:
            m["partitions"].append(parse_partition(v))
        elif n == 15:
            m["groups"] = parse_groups(v)
    return m


# ---- writing (synthetic payloads for tests) ----

def build_payload(partitions, groups, block_size=4096):
    """partitions: list of (name, image_bytes, [(op_type, compressor), ...]).

    Each image is split into one operation per entry in the op list, covering
    consecutive equal-sized block ranges. compressor(bytes) -> bytes.
    """
    blobs = bytearray()
    parts_pb = b""
    for name, image, ops in partitions:
        assert len(image) % block_size == 0
        nblocks = len(image) // block_size
        per = -(-nblocks // len(ops))
        ops_pb = b""
        for i, (op_type, comp) in enumerate(ops):
            start = i * per
            count = min(per, nblocks - start)
            if count <= 0:
                break
            chunk = image[start * block_size:(start + count) * block_size]
            dst = enc_field(1, start) + enc_field(2, count)
            op = enc_field(1, op_type) + enc_field(6, dst)
            if op_type not in (ZERO, DISCARD):
                data = comp(chunk)
                op += enc_field(2, len(blobs)) + enc_field(3, len(data))
                op += enc_field(8, hashlib.sha256(data).digest())
                blobs += data
            ops_pb += enc_field(8, op)
        info = enc_field(1, len(image)) + enc_field(2, hashlib.sha256(image).digest())
        parts_pb += enc_field(13, enc_field(1, name.encode()) + enc_field(7, info) + ops_pb)

    groups_pb = b""
    for gname, gsize, names in groups:
        g = enc_field(1, gname.encode()) + enc_field(2, gsize)
        for n in names:
            g += enc_field(3, n.encode())
        groups_pb += enc_field(1, g)

    manifest = enc_field(3, block_size) + parts_pb + enc_field(15, groups_pb) + enc_field(12, 0)
    header = MAGIC + struct.pack(">QQI", 2, len(manifest), 0)
    return header + manifest + bytes(blobs)
