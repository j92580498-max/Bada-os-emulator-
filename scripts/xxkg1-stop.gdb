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
    if not (0x80000000 <= address and address + length <= 0x90000000):
        raise ValueError('outside guest SDRAM')
    return bytes(inferior.read_memory(ram + address - 0x80000000, length))
ap = gdb.parse_and_eval('$wave->primary.cpu->env')
cp15 = ap['cp15']
sctlr = int(cp15['sctlr_el'][1])
ttbr0 = int(cp15['ttbr0_el'][1])
ttbr1 = int(cp15['ttbr1_el'][1])
print('APPS SCTLR %#x TTBR0 %#x TTBR1 %#x FCSE PID %#x' %
      (sctlr, ttbr0, ttbr1, int(cp15['fcseidr_ns'])))
def word(pa):
    return struct.unpack('<I', read_guest(pa, 4))[0]
def physical(va):
    if not sctlr & 1:
        return va
    n = int(cp15['tcr_el'][1]) & 7
    if n and va >> (32 - n):
        table = ttbr1 & ~0x3fff
    else:
        table = ttbr0 & ~((1 << (14 - n)) - 1)
    desc = word(table + 4 * (va >> 20))
    if desc & 3 == 2:
        return (desc & 0xfff00000) | (va & 0xfffff)
    if desc & 3 == 1:
        page = word((desc & ~0x3ff) + ((va >> 10) & 0x3fc))
        if page & 2:
            return (page & ~0xfff) | (va & 0xfff)
        if page & 3 == 1:
            return (page & ~0xffff) | (va & 0xffff)
    raise ValueError('unmapped virtual address')
task = word(0x82dfe398)
for va in (0x04ffffe8, task, int(ap['regs'][15])):
    try:
        pa = physical(va)
        print('APPS virtual %#x -> physical %#x' % (va, pa))
        if va and va == task and word(pa + 12) in (0x5441534b, 0x48495352):
            name = read_guest(pa + 16, 8).split(b'\0', 1)[0]
            print('APPS scheduled task:', name.decode('ascii', 'backslashreplace'))
    except ValueError:
        pass
def apps_text(reg):
    address = int(ap['regs'][reg])
    if 0x81000000 <= address < 0x81e00000:
        value = read_guest(address, 160).split(b'\0', 1)[0]
        if value and all(32 <= b < 127 for b in value):
            return value.decode('ascii')
    return ''
source = apps_text(4)
message = apps_text(6)
if source.endswith('.c') and message:
    print('APPS diagnostic: %s:%d: %s' % (source, int(ap['regs'][5]), message))
print('IPC shared header:', [hex(i) for i in struct.unpack('<8I', read_guest(0x80f00000, 32))])
for i in range(3):
    bank = gdb.parse_and_eval('$wave->intc.bank[%d]' % i)
    raw = int(bank['software']) | int(bank['edge'])
    wired, pins = int(bank['wired']), int(bank['pins'])
    for bit in range(32):
        mask = 1 << bit
        trigger = (int(bank['trigger'][bit // 8]) >> (4 * (bit % 8))) & 7
        if wired & mask and (trigger == 0 and pins & mask or trigger == 1 and not pins & mask):
            raw |= mask
    print('IRQ bank %d: raw %#x, APPS enabled %#x, AMSS enabled %#x, AMSS FIQ select %#x' %
          (i, raw, int(bank['enable'][0]), int(bank['enable'][1]), int(bank['fiq_select'][1])))
pg5, = struct.unpack('<I', read_guest(0x808b4334, 4))
print('DSP shared page pointers:', [hex(i) for i in struct.unpack('<7I', read_guest(0x808b4320, 28))])
print('AMSS page 5 shared memory:', hex(pg5))
if 0x80000000 <= pg5 < 0x8ffff000:
    producer, consumer = struct.unpack('<HH', read_guest(pg5 + 0x400, 4))
    print('DSP command queue producer/consumer:', producer, consumer)
    if producer < 128 and consumer < 128:
        entries = [struct.unpack('<4H', read_guest(pg5 + 8*((consumer+i)%128), 8))
                   for i in range((producer-consumer)%128)]
        print('Queued DSP commands (ID, count):', [(hex(cmd), sum(1 for e in entries if e[0] == cmd))
              for cmd in sorted({e[0] for e in entries})])
        print('First queued commands:', entries[:18])
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
