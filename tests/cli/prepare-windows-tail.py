#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Place four retained fixture clusters in the tail of a 48 MiB shrink."""

import struct
import sys


def prepare(path):
    with open(path, "r+b") as disk:
        base = 2048 * 512

        def read(offset, size):
            disk.seek(base + offset)
            data = disk.read(size)
            assert len(data) == size, "short fixture read"
            return bytearray(data)

        def write(offset, data):
            disk.seek(base + offset)
            assert disk.write(data) == len(data), "short fixture write"

        def u32(data, offset):
            return struct.unpack_from("<I", data, offset)[0]

        def u64(data, offset):
            return struct.unpack_from("<Q", data, offset)[0]

        boot = read(0, 512)
        assert boot[3:11] == b"EXFAT   " and boot[108] == 9
        assert u64(boot, 72) * 512 == 64 * 1024 * 1024
        spc = 1 << boot[109]
        size = spc * 512
        fat = u32(boot, 80) * 512
        heap = u32(boot, 88) * 512
        count = u32(boot, 92)
        boundary = (48 * 1024 * 1024 - heap) // size + 2

        def offset(cluster):
            assert 2 <= cluster < count + 2, "cluster outside fixture"
            return heap + (cluster - 2) * size

        def chain(first, length, contiguous=False):
            result = []
            for _ in range((length + size - 1) // size):
                assert first not in result, "cyclic fixture chain"
                offset(first)
                result.append(first)
                first = first + 1 if contiguous else u32(read(fat + first * 4, 4), 0)
            if not contiguous:
                assert first >= 0xFFFFFFF8, "wrong fixture chain length"
            return result

        root_cluster = u32(boot, 96)
        root = read(offset(root_cluster), size)
        assert u32(read(fat + root_cluster * 4, 4), 0) >= 0xFFFFFFF8

        def system(kind):
            matches = [i for i in range(0, size, 32) if root[i] == kind]
            assert len(matches) == 1, "missing or duplicate system entry"
            return matches[0]

        bitmap_entry = system(0x81)
        bitmap_cluster = u32(root, bitmap_entry + 20)
        assert u64(root, bitmap_entry + 24) == (count + 7) // 8 <= 512
        assert root_cluster < boundary and bitmap_cluster < boundary
        bitmap = read(offset(bitmap_cluster), size)

        def allocated(cluster):
            return bool(bitmap[(cluster - 2) // 8] & (1 << ((cluster - 2) % 8)))

        def set_bit(cluster, value):
            index, bit = divmod(cluster - 2, 8)
            mask = 1 << bit
            bitmap[index] = (bitmap[index] | mask) if value else (bitmap[index] & ~mask)

        def file_entry(directory, name):
            for i in range(0, len(directory), 32):
                if directory[i] != 0x85:
                    continue
                end = i + (directory[i + 1] + 1) * 32
                entries = directory[i:end]
                names = b"".join(entries[j + 2:j + 32] for j in range(64, len(entries), 32)
                                 if entries[j] == 0xC1)
                if names[:entries[35] * 2].decode("utf-16le") == name:
                    assert entries[32] == 0xC0
                    return i, entries
            raise AssertionError(f"missing fixture entry: {name}")

        def checksum(entries):
            total = 0
            for i, value in enumerate(entries):
                if i not in (2, 3):
                    total = (((total & 1) << 15) + (total >> 1) + value) & 0xFFFF
            struct.pack_into("<H", entries, 2, total)

        def file_chain(entries):
            return chain(u32(entries, 52), u64(entries, 56), bool(entries[33] & 2))

        destinations = [c for c in range(boundary, count + 2) if not allocated(c)][:4]
        assert len(destinations) == 4, "insufficient tail space"
        assert not any(allocated(c) for c in range(boundary, count + 2)), \
            "ordinary fixture already has tail allocations"

        def relocate(clusters, index, destination):
            source = clusters[index]
            assert source < boundary and allocated(source)
            write(offset(destination), read(offset(source), size))
            clusters[index] = destination
            # Materialize the complete chain, including former NoFatChain files.
            for i, cluster in enumerate(clusters):
                successor = clusters[i + 1] if i + 1 < len(clusters) else 0xFFFFFFFF
                write(fat + cluster * 4, struct.pack("<I", successor))
            write(fat + source * 4, bytes(4))
            set_bit(source, False)
            set_bit(destination, True)

        payload_index, payload = file_entry(root, "payload.bin")
        payload_clusters = file_chain(payload)
        relocate(payload_clusters, len(payload_clusters) - 1, destinations[0])
        payload[33] &= ~2
        checksum(payload)
        root[payload_index:payload_index + len(payload)] = payload

        directory_index, directory_entry = file_entry(root, "small-files")
        directory_clusters = file_chain(directory_entry)
        directory = bytearray().join(read(offset(c), size) for c in directory_clusters)
        file_index, small_file = file_entry(directory, "200.txt")
        small_clusters = file_chain(small_file)
        assert len(small_clusters) == 1
        relocate(small_clusters, 0, destinations[1])
        struct.pack_into("<I", small_file, 52, small_clusters[0])
        checksum(small_file)
        directory[file_index:file_index + len(small_file)] = small_file
        for i, cluster in enumerate(directory_clusters):
            write(offset(cluster), directory[i * size:(i + 1) * size])
        relocate(directory_clusters, 0, destinations[2])
        struct.pack_into("<I", directory_entry, 52, directory_clusters[0])
        directory_entry[33] &= ~2
        checksum(directory_entry)
        root[directory_index:directory_index + len(directory_entry)] = directory_entry

        upcase_index = system(0x82)
        upcase_clusters = chain(u32(root, upcase_index + 20), u64(root, upcase_index + 24))
        relocate(upcase_clusters, len(upcase_clusters) - 1, destinations[3])
        struct.pack_into("<I", root, upcase_index + 20, upcase_clusters[0])

        assert sum(allocated(c) for c in range(boundary, count + 2)) == 4
        write(offset(root_cluster), root)
        write(offset(bitmap_cluster), bitmap)
        # Allocated cluster count and boot geometry are unchanged. The following
        # independent fsck/read-only mount in the caller validates the edited image.
        print("prepare-windows-tail: payload tail, file head, directory and upcase at "
              + ", ".join(map(str, destinations)))


if __name__ == "__main__":
    prepare(sys.argv[1])
