#!/usr/bin/env python3
"""Prepare the pinned x86-64 Mach-O runtime using only the Python standard library."""
import argparse
import hashlib
import pathlib
import struct

FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_X86_64 = 0x01000007
MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_DYLD_INFO_ONLY = 0x80000022
LC_DYLD_CHAINED_FIXUPS = 0x80000034
EXPECTED = 'a68a92300cc38e5941f59832c516a37eb098c10c6bbe47ab3dcbe812705cf5ee'


def cstring(data, offset, limit=None):
    end = data.find(b'\0', offset, limit)
    if end < 0:
        raise ValueError('unterminated string in Mach-O metadata')
    return data[offset:end].decode('utf-8', errors='strict')


def fixed_name(value):
    return value.split(b'\0', 1)[0].decode('ascii')


def uleb(data, offset, limit):
    value = 0
    shift = 0
    while offset < limit and shift < 64:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
    raise ValueError('invalid ULEB128 in dyld metadata')


def sleb(data, offset, limit):
    value = 0
    shift = 0
    while offset < limit and shift < 64:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if not byte & 0x80:
            if shift < 64 and byte & 0x40:
                value |= -(1 << shift)
            return value, offset
    raise ValueError('invalid SLEB128 in dyld metadata')


def parse_bind_stream(data, start, size, segments, symbols, weak=False, lazy=False):
    if not size:
        return []
    offset = start
    limit = start + size
    segment = 0
    address_offset = 0
    symbol = ''
    addend = 0
    output = []

    def bind():
        if segment >= len(segments) or not symbol:
            raise ValueError('incomplete dyld bind state')
        address = (segments[segment]['vmaddr'] + address_offset) & 0xffffffffffffffff
        value = symbols.get(symbol, 0)
        if weak and value:
            output.append(f'OWN {address:x} {value:x} {addend}')
        else:
            output.append(f'BIND {address:x} {addend} {symbol}')

    while offset < limit:
        byte = data[offset]
        offset += 1
        opcode, immediate = byte & 0xf0, byte & 0x0f
        if opcode == 0x00:  # DONE; lazy streams contain multiple records.
            if not lazy:
                break
            segment = 0
            address_offset = 0
            symbol = ''
            addend = 0
        elif opcode == 0x10:  # SET_DYLIB_ORDINAL_IMM
            pass
        elif opcode == 0x20:  # SET_DYLIB_ORDINAL_ULEB
            _, offset = uleb(data, offset, limit)
        elif opcode == 0x30:  # SET_DYLIB_SPECIAL_IMM
            pass
        elif opcode == 0x40:  # SET_SYMBOL_TRAILING_FLAGS_IMM
            symbol = cstring(data, offset, limit)
            offset += len(symbol.encode()) + 1
        elif opcode == 0x50:  # SET_TYPE_IMM
            pass
        elif opcode == 0x60:  # SET_ADDEND_SLEB
            addend, offset = sleb(data, offset, limit)
        elif opcode == 0x70:  # SET_SEGMENT_AND_OFFSET_ULEB
            segment = immediate
            address_offset, offset = uleb(data, offset, limit)
        elif opcode == 0x80:  # ADD_ADDR_ULEB
            value, offset = uleb(data, offset, limit)
            address_offset = (address_offset + value) & 0xffffffffffffffff
        elif opcode == 0x90:  # DO_BIND
            bind()
            address_offset = (address_offset + 8) & 0xffffffffffffffff
        elif opcode == 0xa0:  # DO_BIND_ADD_ADDR_ULEB
            bind()
            value, offset = uleb(data, offset, limit)
            address_offset = (address_offset + 8 + value) & 0xffffffffffffffff
        elif opcode == 0xb0:  # DO_BIND_ADD_ADDR_IMM_SCALED
            bind()
            address_offset = (address_offset + 8 + immediate * 8) & 0xffffffffffffffff
        elif opcode == 0xc0:  # DO_BIND_ULEB_TIMES_SKIPPING_ULEB
            count, offset = uleb(data, offset, limit)
            skip, offset = uleb(data, offset, limit)
            for _ in range(count):
                bind()
                address_offset = (address_offset + 8 + skip) & 0xffffffffffffffff
        else:
            raise ValueError(f'unsupported dyld bind opcode 0x{byte:02x}')
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    raw = args.binary.read_bytes()
    if hashlib.sha256(raw).hexdigest() != EXPECTED:
        raise ValueError('unsupported original engine build')
    magic, count = struct.unpack_from('>II', raw)
    if magic != FAT_MAGIC or count >= 16:
        raise ValueError('invalid universal Mach-O container')
    architectures = [struct.unpack_from('>IIIII', raw, 8 + i * 20)
                     for i in range(count)]
    arch = next((item for item in architectures if item[0] == CPU_TYPE_X86_64), None)
    if not arch:
        raise ValueError('x86-64 Mach-O slice is missing')
    thin = raw[arch[2]:arch[2] + arch[3]]
    if len(thin) != arch[3]:
        raise ValueError('truncated x86-64 Mach-O slice')

    header = struct.unpack_from('<8I', thin)
    if header[0] != MH_MAGIC_64 or header[1] != CPU_TYPE_X86_64:
        raise ValueError('invalid x86-64 Mach-O header')
    ncmds, sizeofcmds = header[4], header[5]
    if 32 + sizeofcmds > len(thin):
        raise ValueError('truncated Mach-O load commands')

    segments = []
    sections = []
    symtab = None
    dyld = None
    cursor = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', thin, cursor)
        if cmdsize < 8 or cursor + cmdsize > 32 + sizeofcmds:
            raise ValueError('invalid Mach-O load command')
        if cmd == LC_DYLD_CHAINED_FIXUPS:
            raise ValueError('dyld chained fixups are not supported')
        if cmd == LC_SEGMENT_64:
            values = struct.unpack_from('<II16sQQQQiiII', thin, cursor)
            segment = dict(name=fixed_name(values[2]), vmaddr=values[3],
                           vmsize=values[4], fileoff=values[5], filesize=values[6],
                           initprot=values[8], nsects=values[9])
            segments.append(segment)
            section_cursor = cursor + 72
            if section_cursor + segment['nsects'] * 80 > cursor + cmdsize:
                raise ValueError('truncated Mach-O section list')
            for index in range(segment['nsects']):
                item = struct.unpack_from('<16s16sQQIIIIIIII', thin,
                                          section_cursor + index * 80)
                sections.append(dict(name=fixed_name(item[0]),
                                     segment=fixed_name(item[1]),
                                     address=item[2], size=item[3], offset=item[4]))
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from('<6I', thin, cursor)[2:]
        elif cmd == LC_DYLD_INFO_ONLY:
            dyld = struct.unpack_from('<12I', thin, cursor)[2:]
        cursor += cmdsize

    if not symtab or not dyld:
        raise ValueError('required Mach-O metadata is missing')
    symoff, nsyms, stroff, strsize = symtab
    if symoff + nsyms * 16 > len(thin) or stroff + strsize > len(thin):
        raise ValueError('truncated Mach-O symbol table')
    names = {}
    symbol_rows = []
    for index in range(nsyms):
        strx, _, _, _, value = struct.unpack_from('<IBBHQ', thin, symoff + index * 16)
        if not strx or strx >= strsize:
            continue
        name = cstring(thin, stroff + strx, stroff + strsize)
        symbol_rows.append((value, name))
        if value and name:
            names[value] = name
    symbols_by_name = {name: value for value, name in symbol_rows if value and name}

    lines = []
    for segment in segments:
        if segment['name'] == '__PAGEZERO':
            continue
        lines.append('SEG {vmaddr:x} {vmsize:x} {fileoff:x} {filesize:x} '
                     '{initprot} {name}'.format(**segment))

    rebase_off, rebase_size, bind_off, bind_size, weak_off, weak_size, \
        lazy_off, lazy_size, _, _ = dyld
    del rebase_off, rebase_size
    lines += parse_bind_stream(thin, bind_off, bind_size, segments, symbols_by_name)
    lines += parse_bind_stream(thin, weak_off, weak_size, segments,
                               symbols_by_name, weak=True)
    lines += parse_bind_stream(thin, lazy_off, lazy_size, segments,
                               symbols_by_name, lazy=True)

    for value, name in symbol_rows:
        if value and name and (name.startswith(('_wxime_', '_business_', '_net_'))
                               or 'loguru' in name):
            lines.append(f'SYM {value:x} {name}')

    imagebase = next(segment['vmaddr'] for segment in segments
                     if segment['name'] != '__PAGEZERO' and segment['fileoff'] == 0)
    for section in sections:
        if section['name'] in ('__thread_vars', '__thread_data', '__thread_bss', '__eh_frame'):
            lines.append(f"SECTION {section['address']:x} {section['size']:x} {section['name']}")
        content = thin[section['offset']:section['offset'] + section['size']]
        if len(content) != section['size']:
            raise ValueError(f"truncated section {section['name']}")
        if section['name'] == '__mod_init_func':
            for (value,) in struct.iter_unpack('<Q', content):
                lines.append(f'CTOR {value:x} {names.get(value, "unnamed")}')
        if section['name'] == '__unwind_info':
            version, common_off, common_n, _, _, index_off, index_n = \
                struct.unpack_from('<7I', content)
            if version != 1:
                raise ValueError('unsupported compact unwind version')
            common = struct.unpack_from(f'<{common_n}I', content, common_off)
            indices = [struct.unpack_from('<III', content, index_off + i * 12)
                       for i in range(index_n)]
            lsdas = {}
            unwind = []
            for position, (first, page, lsda_off) in enumerate(indices[:-1]):
                for item in range(lsda_off, indices[position + 1][2], 8):
                    function, lsda = struct.unpack_from('<II', content, item)
                    lsdas[function] = imagebase + lsda
                kind, = struct.unpack_from('<I', content, page)
                if kind == 2:
                    entry_off, entry_n = struct.unpack_from('<HH', content, page + 4)
                    unwind.extend(struct.unpack_from('<II', content,
                                  page + entry_off + i * 8) for i in range(entry_n))
                elif kind == 3:
                    entry_off, entry_n, encoding_off, encoding_n = \
                        struct.unpack_from('<4H', content, page + 4)
                    encodings = list(common) + list(struct.unpack_from(
                        f'<{encoding_n}I', content, page + encoding_off))
                    for i in range(entry_n):
                        item, = struct.unpack_from('<I', content,
                                                   page + entry_off + i * 4)
                        unwind.append((first + (item & 0xffffff),
                                       encodings[item >> 24]))
                else:
                    raise ValueError(f'unsupported unwind page {kind}')
            unwind.sort()
            for index, (function, encoding) in enumerate(unwind):
                end = unwind[index + 1][0] if index + 1 < len(unwind) else indices[-1][0]
                if ((encoding >> 24) & 15) == 1:
                    lines.append(f'UNWIND {imagebase + function:x} {end - function:x} '
                                 f'{encoding:x} {lsdas.get(function, 0):x}')

    debug_names = {value: name for value, name in symbol_rows
                   if value and name and
                   (name.startswith(('_wxime_', '_business_', '_net_')) or
                    'SetReportExceptionCallBack' in name)}
    (args.output / 'image.macho').write_bytes(thin)
    (args.output / 'manifest.txt').write_text('\n'.join(lines) + '\n')
    (args.output / 'symbols.txt').write_text('\n'.join(
        f'{value:x} {name}' for value, name in sorted(debug_names.items())) + '\n')
    (args.output / 'sha256.txt').write_text(hashlib.sha256(raw).hexdigest() + '\n')
    print(f'Prepared {len(thin)} byte x86-64 image; metadata only.')


if __name__ == '__main__':
    main()
