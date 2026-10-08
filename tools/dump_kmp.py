import struct

with open(r"C:\Users\dwain\Kinoko\web\assets\tracks\beginner_course\course.szs", "rb") as f:
    data = f.read()

# 1. Decompress Yaz0
if data[:4] == b"Yaz0":
    uncompressed_size = struct.unpack(">I", data[4:8])[0]
    src = 16
    dst = bytearray()
    while src < len(data) and len(dst) < uncompressed_size:
        flag = data[src]; src += 1
        for i in range(8):
            if len(dst) >= uncompressed_size: break
            if flag & (0x80 >> i):
                dst.append(data[src]); src += 1
            else:
                b1, b2 = data[src], data[src+1]; src += 2
                dist = ((b1 & 0x0F) << 8) | b2
                length = (b1 >> 4) + 2 if (b1 >> 4) else data[src] + 0x12
                if not (b1 >> 4): src += 1
                start = len(dst) - (dist + 1)
                for _ in range(length):
                    dst.append(dst[start]); start += 1
    data = bytes(dst)

# 2. Find course.kmp inside U8
node_count = struct.unpack(">I", data[12:16])[0] // 12
string_pool = 16 + node_count * 12
kmp_data = None
for i in range(node_count):
    node = data[16 + i*12 : 28 + i*12]
    if node[0] == 0:
        name_off = struct.unpack(">I", b"\x00" + node[1:4])[0]
        end = data.find(b"\x00", string_pool + name_off)
        name = data[string_pool + name_off : end].decode("ascii", "ignore")
        if name == "course.kmp":
            off = struct.unpack(">I", node[4:8])[0]
            size = struct.unpack(">I", node[8:12])[0]
            kmp_data = data[off : off + size]
            break

# 3. Read GOBJ Section in KMP
if kmp_data and kmp_data[:4] == b"RKMD":
    sec_count = struct.unpack(">H", kmp_data[8:10])[0]
    sec_offsets = [struct.unpack(">I", kmp_data[16 + i*4 : 20 + i*4])[0] for i in range(sec_count)]
    for off in sec_offsets:
        if kmp_data[off : off+4] == b"GOBJ":
            count = struct.unpack(">H", kmp_data[off+4 : off+6])[0]
            print(f"Total GOBJ entries in KMP: {count}")
            seen = {}
            for j in range(count):
                entry = kmp_data[off + 8 + j*60 : off + 8 + (j+1)*60]
                obj_id = struct.unpack(">H", entry[:2])[0]
                x, y, z = struct.unpack(">fff", entry[4:16])
                hex_id = f"0x{obj_id:x}"
                seen[hex_id] = seen.get(hex_id, 0) + 1
                if seen[hex_id] == 1:
                    print(f"ID={hex_id:<6} ({obj_id:3d}) | Pos=[{x:8.1f}, {y:8.1f}, {z:8.1f}]")
            print("Total Counts per ID:", seen)
