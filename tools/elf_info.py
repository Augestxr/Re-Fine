"""elf_info.py - segmentos, secciones y --code-end de un ELF PPU (EBOOT.elf).

Imprime la cabecera, los program headers y la tabla de secciones con sus flags, y
calcula el --code-end para ppu_lifter.py: el fin (exclusivo) de la ultima seccion
con SHF_EXECINSTR (como en docs/decisions.md de f2nd-recomp). Las secciones del
EBOOT no tienen nombre, asi que se decide por flags.

Con --scan busca ademas, en todo el archivo, cabeceras ELF (\\x7FELF) y SCE (SCE\\0)
embebidas despues del offset 0, con el e_machine de cada ELF (21 = PPC64, 23 = SPU).

    python tools/elf_info.py game/EBOOT.elf [--scan]
"""

import argparse
import struct

SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 0x1, 0x2, 0x4


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('elf')
    ap.add_argument('--scan', action='store_true', help='buscar ELF/SCE embebidos')
    a = ap.parse_args()
    d = open(a.elf, 'rb').read()
    if d[:4] != b'\x7fELF':
        raise SystemExit(f'{a.elf}: no es ELF (magic {d[:4].hex()})')
    if d[4] != 2 or d[5] != 2:
        raise SystemExit('se espera ELF64 big-endian')

    (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags, _ehsize,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('>HHIQQQIHHHHHH', d, 16)
    print(f'archivo      : {a.elf} ({len(d):,} bytes)')
    print(f'e_type       : {e_type}   e_machine: {e_machine}   e_entry (OPD): {e_entry:#x}')
    print(f'program hdrs : {e_phnum} en {e_phoff:#x}    section hdrs: {e_shnum} en {e_shoff:#x} (shstrndx {e_shstrndx})')

    print('\n-- program headers')
    print(f"{'#':>2} {'tipo':>10} {'offset':>10} {'vaddr':>10} {'filesz':>10} {'memsz':>10} flags")
    for i in range(e_phnum):
        p_type, p_flags, p_off, p_vaddr, _pa, p_filesz, p_memsz, _al = struct.unpack_from('>IIQQQQQQ', d, e_phoff + i * e_phentsize)
        print(f'{i:>2} {p_type:>#10x} {p_off:>#10x} {p_vaddr:>#10x} {p_filesz:>#10x} {p_memsz:>#10x} {p_flags:#x}')

    print('\n-- secciones (sin nombre: se clasifican por flags)')
    print(f"{'#':>3} {'tipo':>6} {'flags':>6} {'addr':>10} {'fin':>10} {'tamano':>10}  clase")
    code_end = 0
    for i in range(e_shnum):
        (sh_name, sh_type, sh_flags, sh_addr, sh_off, sh_size, _l, _inf, _al, _es) = struct.unpack_from('>IIQQQQIIQQ', d, e_shoff + i * e_shentsize)
        if sh_type == 0:
            continue
        if sh_flags & SHF_EXECINSTR:
            cls = 'codigo (X)'
            code_end = max(code_end, sh_addr + sh_size)
        elif sh_flags & SHF_WRITE:
            cls = 'datos (W)' if sh_type != 8 else 'bss (W, NOBITS)'
        elif sh_flags & SHF_ALLOC:
            cls = 'solo lectura'
        else:
            cls = 'no se carga'
        print(f'{i:>3} {sh_type:>#6x} {sh_flags:>#6x} {sh_addr:>#10x} {sh_addr + sh_size:>#10x} {sh_size:>#10x}  {cls}')
    print(f'\n--code-end (fin exclusivo de la ultima seccion SHF_EXECINSTR): {code_end:#x}')

    if a.scan:
        print('\n-- cabeceras embebidas (offset > 0)')
        for magic, what in ((b'\x7fELF', 'ELF'), (b'SCE\x00', 'SCE')):
            pos = d.find(magic, 1)
            while pos != -1:
                extra = ''
                if what == 'ELF' and pos + 20 <= len(d):
                    cls, data = d[pos + 4], d[pos + 5]
                    mach = struct.unpack_from('>H', d, pos + 18)[0] if data == 2 else struct.unpack_from('<H', d, pos + 18)[0]
                    extra = f'ELFCLASS{32 if cls == 1 else 64 if cls == 2 else cls} e_machine={mach}'
                print(f'  {what} en offset {pos:#x} {extra}')
                pos = d.find(magic, pos + 1)


if __name__ == '__main__':
    main()
