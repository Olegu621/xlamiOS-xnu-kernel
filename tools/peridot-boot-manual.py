#!/usr/bin/env python3
"""Temporarily boot a manual-return image; preserve the running phone afterwards."""
import argparse,hashlib,json,struct,subprocess,time
from pathlib import Path
def run(args,timeout=20):
 return subprocess.run(args,capture_output=True,check=True,timeout=timeout)
def main():
 ap=argparse.ArgumentParser();ap.add_argument('image',type=Path);ap.add_argument('out',type=Path);a=ap.parse_args()
 blob=a.image.read_bytes()
 if blob[:8]!=b'ANDROID!':raise ValueError('Expected Android boot image')
 page=struct.unpack_from('<I',blob,36)[0];kernel=struct.unpack_from('<I',blob,8)[0]
 if page not in [2048,4096,8192,16384]:raise ValueError('Unsupported page size')
 offset=page+((kernel+page-1)//page)*page
 if blob[offset:offset+8]!=b'SWEETXNU':raise ValueError('Expected XNU payload')
 cmd=blob[offset+0x48:offset+0x2a8].split(b'\0',1)[0].decode()
 args=dict(x.split('=',1) for x in cmd.split() if '=' in x)
 if args.get('sweetdiagreset')!='0' or args.get('sweetpanicreset')!='0' or args.get('peridotmanual')!='1':raise ValueError('Image lacks explicit manual-return flags')
 rows=[x.split() for x in run(['adb','devices']).stdout.decode().splitlines()[1:] if x.strip()]
 if len(rows)!=1 or rows[0][1]!='recovery':raise ValueError('Need exactly one recovery device')
 serial=rows[0][0];a.out.mkdir(parents=True,exist_ok=True)
 meta={'image_sha256':hashlib.sha256(blob).hexdigest(),'cmdline':cmd,'temporary_boot_accepted':False,'visual_boot_confirmed':False}
 (a.out/'boot.json').write_text(json.dumps(meta,indent=2))
 run(['adb','-s',serial,'reboot','bootloader'])
 for _ in range(45):
  if serial in run(['fastboot','devices']).stdout.decode():break
  time.sleep(1)
 else:raise RuntimeError('Fastboot did not appear')
 result=run(['fastboot','-s',serial,'boot',str(a.image.resolve())],60)
 (a.out/'fastboot-boot.txt').write_bytes(result.stdout+result.stderr)
 meta['temporary_boot_accepted']=True
 (a.out/'boot.json').write_text(json.dumps(meta,indent=2))
 print('Temporary boot accepted; no reset timer, no recovery polling or reboot command follows.')
if __name__=='__main__':main()
