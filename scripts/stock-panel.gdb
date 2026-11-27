set pagination off
set confirm off
set print thread-events off
tbreak wave723_finish
run
# First argument to wave723_finish on the hosted AArch64 runner.
set $panel = &((Wave723State *)$x0)->lcd
python
import gdb
import os
panel = gdb.parse_and_eval('$panel').dereference()
pixels = panel['rgb']
data = gdb.selected_inferior().read_memory(int(pixels.address), pixels.type.sizeof)
with open(os.environ['WAVE723_PANEL_SAMPLE'], 'wb') as sample:
    sample.write(data)
print('Sampled panel RAM: %d pixel writes, sleep-out %d, display-on %d' %
      (int(panel['writes']), int(panel['awake']), int(panel['display_on'])))
end
continue
