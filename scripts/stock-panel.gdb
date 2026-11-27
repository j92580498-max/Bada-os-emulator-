set pagination off
set confirm off
set print thread-events off
python
import os, sys, struct, gdb
sys.path.append('/usr/lib/python3/dist-packages')
import capstone
arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
def mrptr(expr):
    return int(gdb.parse_and_eval('(uintptr_t)memory_region_get_ram_ptr(&' + expr + ')'))
class Budget(gdb.Breakpoint):
    def stop(self):
        secs = int(os.environ['WAVE723_PROBE_SECONDS'])
        gdb.execute('set ((Wave723State *)$x0)->limit = %d' % (secs * 1000000000))
        return False
class PanelSample(gdb.Breakpoint):
    def stop(self):
        w = gdb.parse_and_eval('(Wave723State *)$x0')
        gdb.set_convenience_variable('board', w)
        inf = gdb.selected_inferior()
        lcd = w['lcd']
        if 'WAVE723_PANEL_SAMPLE' in os.environ:
            pixels = lcd['rgb']
            with open(os.environ['WAVE723_PANEL_SAMPLE'], 'wb') as f:
                f.write(inf.read_memory(int(pixels.address), pixels.type.sizeof))
        print('Panel writes=%d awake=%d visible=%d' % tuple(int(lcd[k]) for k in ['writes','awake','display_on']))
        ram = int(gdb.parse_and_eval('(uintptr_t)memory_region_get_ram_ptr($board->parent_obj.ram)'))
        def rb(addr,n):
            if not 0x80000000 <= addr <= 0x90000000-n:
                raise ValueError(hex(addr))
            return bytes(inf.read_memory(ram+addr-0x80000000,n))
        def w32(addr):
            return struct.unpack('<I',rb(addr,4))[0]
        for core in ['primary','modem']:
            e = w[core]['cpu']['env']
            cp15 = e['cp15']
            mmu = int(cp15['sctlr_el'][1]) & 1
            regs = [int(x) for x in [e['regs'][i] for i in range(16)]]
            print(core,'registers', ' '.join('%x' % n for n in regs))
            windows = [(0x80000000, 0x10000000, ram)]
            if core == 'modem':
                windows.extend([(0x10000,0x10000,mrptr('$board->modem_code_sram')),(0x08708000,0x8000,mrptr('$board->diagnostic_stack'))])
            else:
                windows.append((0xffff0000,0x8000,mrptr('$board->high_sram')))
            for i in range(2):
                tcm = w[core]['tcm'][i]
                r = int(tcm['region'])
                if r & 1:
                    sz = 512 << ((r >> 2) & 0xf)
                    windows.insert(0,(r & -sz,sz,mrptr('$board->%s.tcm[%d].memory' % (core,i))))
            def tr(va):
                if not mmu: return va
                n = int(cp15['tcr_el'][1]) & 7
                tt = int(cp15['ttbr0_el'][1]) & ~((1 << (14-n))-1)
                if n and va >> (32-n):
                    tt = int(cp15['ttbr1_el'][1]) & ~0x3fff
                desc = w32(tt+4*(va >> 20))
                if desc & 3 == 2: return (desc & 0xfff00000)|(va & 0xfffff)
                if desc & 3 == 1:
                    page=w32((desc & ~0x3ff)+((va >> 10) & 0x3fc))
                    if page & 2: return (page & ~0xfff)|(va & 0xfff)
                    if page & 3 == 1: return (page & ~0xffff)|(va & 0xffff)
                raise ValueError(hex(va))
            def read(va,n):
                p=tr(va)
                for lo,sz,host in windows:
                    if lo <= p <= lo+sz-n:
                        return bytes(inf.read_memory(host+p-lo,n))
                raise ValueError(hex(p))
            def dis(va,t,n):
                try:
                    return ' | '.join('%08x %s %s' % (i.address,i.mnemonic,i.op_str) for i in (thumb if t else arm).disasm(read(va,n),va))
                except (ValueError, gdb.MemoryError):
                    return '?'
            pc,lr,sp=regs[15],regs[14],regs[13]
            t = bool(e['thumb'])
            for a,ts,sz in ((max(0,pc-16) & ~1,t,56),((lr & ~1)-12,lr & 1,22)):
                print(core,'code',dis(a,ts,sz))
            try:
                words=struct.unpack('<96I',read(sp,384))
                print(core,'stack code', ' '.join(hex(n) for n in words if 0x80300000 <= n < 0x81e00000)[:900])
                if core=='primary':
                    for reg in (4,6):
                        n=regs[reg]
                        if 0x81000000 <= n < 0x81e00000:
                            print(core,'text',read(n,96).split(b'\0')[0])
            except (ValueError,gdb.MemoryError): pass
        print('CP shared queue head:',rb(0x8000c400,4).hex(), 'IPC:',rb(0x80f00000,32).hex())
        for a,n in [(0x8000c000,0x400)]:
            cmd=struct.unpack_from('<H',rb(0x8000c402,2))[0];prod=struct.unpack_from('<H',rb(0x8000c400,2))[0]
            print('DSP pending',[struct.unpack('<4H',rb(a+8*((cmd+j)%128),8)) for j in range(min(24,(prod-cmd)%128))])
        if w32(0x808b3664)==0xca:
            args=struct.unpack('<4I',rb(0x808b3814,16))
            print('CP assert', [hex(n) for n in args], [rb(n,100).split(b'\0')[0] for n in args if 0x80300000<=n<0x81000000])
        return False
Budget('wave723_reset',internal=True)
PanelSample('wave723_finish',internal=True)
end
run
