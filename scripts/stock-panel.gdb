set pagination off
set confirm off
set print thread-events off
python
import gdb
import os
class PanelSample(gdb.Breakpoint):
    def stop(self):
        # First argument to wave723_finish on the hosted AArch64 runner.
        panel = gdb.parse_and_eval('((Wave723State *)$x0)->lcd')
        pixels = panel['rgb']
        data = gdb.selected_inferior().read_memory(int(pixels.address), pixels.type.sizeof)
        with open(os.environ['WAVE723_PANEL_SAMPLE'], 'wb') as sample:
            sample.write(data)
        print('Sampled panel RAM: %d pixel writes, sleep-out %d, display-on %d' %
              (int(panel['writes']), int(panel['awake']), int(panel['display_on'])))
        return False
PanelSample('wave723_finish', internal=True)
end
run
