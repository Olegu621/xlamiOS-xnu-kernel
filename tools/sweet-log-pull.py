import argparse,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('-s',required=True);p.add_argument('out');a=p.parse_args()
adb=['adb','-s',a.s]
def run(args):return subprocess.run(adb+args,check=True,capture_output=True,timeout=60).stdout
dev='/dev/block/by-name/userdata';size=int(run(['shell','blockdev','--getsize64',dev]));assert size>1025*4096
raw=run(['exec-out',f'dd if={dev} bs=4096 skip={size//4096-1025} count=1025 2>/dev/null']);assert len(raw)==1025*4096
out=Path(a.out);binpath=Path(str(out)+'.bin');binpath.write_bytes(raw)
subprocess.run([sys.executable,str(Path(__file__).with_name('sweet-log.py')),str(binpath),str(out)],check=True)
