"""Run the production Radio buffer admission gate offline."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

# Execute each production buffer gate with a paced HTTP reader. The initial
# frame is now admitted after two 4 KiB chunks, retaining the same refill gate.
gates = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#define pdMS_TO_TICKS(x) (x)
struct Buffer {std::size_t bytes;std::size_t available(){return bytes;}};
struct Reader {std::atomic<bool> finished;Buffer buffer;};
static Reader *active;static unsigned waits;
static void vTaskDelay(unsigned){assert(++waits<8);active->buffer.bytes+=4096;}
'''
for name, path, marker in (
    ('radio', 'main/apps/radio/radio_player.cc', '            buffering = false;'),
):
    player = (root / path).read_text()
    start = player.index('const bool finished = reader.finished.load')
    end = player.index(marker, start)
    gates += f'bool {name}(size_t bytes,bool finished){{Reader reader{{finished,{{bytes}}}};active=&reader;waits=0;'
    gates += 'constexpr size_t capacity=16384;bool buffering=true;while(true){'
    gates += player[start:end] + 'return true;}return false;}\n'
gates += r'''
int main(){
 for(auto admit:{radio}){
  assert(admit(0,false)&&waits==2);
  assert(admit(4096,false)&&waits==1);
  assert(admit(8192,false)&&waits==0);
  assert(admit(1024,true)&&waits==0);
  assert(!admit(0,true)&&waits==0);
 }
 puts("Radio production buffer gate: paced 8 KiB start/refill, short EOF, empty EOF PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='passport-media-startup-') as temporary:
    for name, text in (('gates', gates),):
        src = Path(temporary) / (name + '.cc')
        exe = src.with_suffix('')
        src.write_text(text)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-fsanitize=address,undefined', '-g',
                        '-Itests/registry_mocks', '-Imain', '-Icomponents/bsp/include', str(src), '-o', str(exe)], cwd=root, check=True)
        subprocess.run([str(exe)], cwd=root, check=True)
