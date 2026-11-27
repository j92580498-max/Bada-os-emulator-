set pagination off
set confirm off
set print thread-events off
break wave723_finish
run
# First argument to the QEMU machine callback on the hosted AArch64 runner.
set $wave = (Wave723State *)$x0
set $ram = (unsigned char *)memory_region_get_ram_ptr($wave->parent_obj.ram)
printf "APPS registers at stop\n"
p/x $wave->primary.cpu->env.regs
printf "AMSS registers at stop\n"
p/x $wave->modem.cpu->env.regs
python
import gdb
import struct
ram = int(gdb.parse_and_eval('$ram'))
inferior = gdb.selected_inferior()
def read_guest(address, length):
    return bytes(inferior.read_memory(ram + address - 0x80000000, length))
ap = gdb.parse_and_eval('$wave->primary.cpu->env')
cp15 = ap['cp15']
sctlr = int(cp15['sctlr_el'][1])
ttbr0 = int(cp15['ttbr0_el'][1])
ttbr1 = int(cp15['ttbr1_el'][1])
print('APPS SCTLR %#x TTBR0 %#x TTBR1 %#x FCSE PID %#x' %
      (sctlr, ttbr0, ttbr1, int(cp15['fcseidr_ns'])))
if 0x80000000 <= (ttbr0 & ~0x3fff) <= 0x8fffc000:
    for va in (0x04ffffe8, int(ap['regs'][0]), int(ap['regs'][15])):
        slot = (ttbr0 & ~0x3fff) + 4 * (va >> 20)
        desc, = struct.unpack('<I', read_guest(slot, 4))
        print('TTBR0 first-level descriptor for %#x at %#x: %#x' % (va, slot, desc))
pg5, = struct.unpack('<I', read_guest(0x808b4334, 4))
print('AMSS page 5 shared memory:', hex(pg5))
if 0x80000000 <= pg5 < 0x8ffff000:
    producer, consumer = struct.unpack('<HH', read_guest(pg5 + 0x400, 4))
    print('DSP command queue producer/consumer:', producer, consumer)
    if producer < 128 and consumer < 128:
        entries = [struct.unpack('<4H', read_guest(pg5 + 8*((consumer+i)%128), 8))
                   for i in range((producer-consumer)%128)]
        print('Queued DSP commands (ID, count):', [(hex(cmd), sum(1 for e in entries if e[0] == cmd))
              for cmd in sorted({e[0] for e in entries})])
        print('First queued commands:', entries[:6])
# XXKG1 __xassert sets this flag and saves its four arguments at 0x80626484.
if read_guest(0x808b3664, 4) == bytes((0xca,0,0,0)):
    args = struct.unpack('<4I', read_guest(0x808b3814, 16))
    print('AMSS __xassert arguments:', *(hex(value) for value in args))
    for address in args:
        if 0x80300000 <= address < 0x81000000:
            value = read_guest(address, 160).split(b'\0', 1)[0]
            if value and all(32 <= b < 127 for b in value):
                print('AMSS __xassert:', value.decode('ascii'))
end
disable 1
continue
