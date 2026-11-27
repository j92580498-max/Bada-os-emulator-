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
