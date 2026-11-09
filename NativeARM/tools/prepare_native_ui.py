"""Generate native-compatible UI locally from the owner's Alyx VPK; no game assets shipped."""
from pathlib import Path
import re,struct,zlib

def entries(path):
    with path.open('rb') as f:
        header=f.read(28);magic,version,length=struct.unpack_from('<III',header)
        if magic!=0x55aa1234 or version not in (1,2):raise ValueError('Unsupported Alyx VPK')
        start=28 if version==2 else 12;f.seek(start);b=f.read(length)
    i=0
    def string():
        nonlocal i
        end=b.index(0,i);s=b[i:end].decode();i=end+1;return s
    while True:
        ext=string()
        if not ext:return
        while True:
            folder=string()
            if not folder:break
            while True:
                name=string()
                if not name:break
                crc,preload,archive,offset,size,end=struct.unpack_from('<IHHIIH',b,i);i+=18
                if end!=65535:raise ValueError('Invalid VPK entry')
                extra=b[i:i+preload];i+=preload
                key=('' if folder==' ' else folder+'/')+name+('' if ext==' ' else '.'+ext)
                yield key,(archive,offset,size,extra,start+length)

def read(path,item):
    archive,offset,size,extra,base=item
    target=path if archive==0x7fff else path.with_name(path.name.replace('_dir.vpk',f'_{archive:03}.vpk'))
    with target.open('rb') as f:f.seek(offset+(base if archive==0x7fff else 0));return extra+f.read(size)

def patch_resource(blob,xml):
    b=bytearray(blob)
    if len(b)<16:raise ValueError('Truncated Panorama resource')
    table=8+struct.unpack_from('<I',b,8)[0];count=struct.unpack_from('<I',b,12)[0]
    for n in range(count):
        at=table+n*12
        if b[at:at+4]!=b'DATA':continue
        offset=at+4+struct.unpack_from('<I',b,at+4)[0];size=struct.unpack_from('<I',b,at+8)[0]
        if offset+size!=len(b):raise ValueError('Unexpected Panorama block layout')
        crc=struct.unpack_from('<I',b,offset)[0]
        start=next((i for i in range(offset+4,min(offset+8192,len(b))) if zlib.crc32(b[i:])==crc),None)
        if start is None:raise ValueError('Cannot locate verified Panorama text')
        text=bytes(b[start:]);name=b'GameMenuOptionRenderingFoveatedTogglePanel'
        if xml:
            text=re.sub(rb'<'+name+rb'\b[^>]*?/>',lambda m:b'<!-- Native compatibility -->'.ljust(len(m[0]),b' '),text,flags=re.S)
        else:text=text.replace(name,b'.mvr_unused_foveation'.ljust(len(name),b'_'))
        if len(text)!=len(b)-start:raise ValueError('Panorama patch changed resource size')
        b[start:]=text;struct.pack_into('<I',b,offset,zlib.crc32(text));return bytes(b)
    raise ValueError('Missing Panorama DATA block')

def write_vpk(path,files):
    tree=bytearray();data=bytearray()
    for key,b in files.items():
        folder,name=key.rsplit('/',1);stem,ext=name.rsplit('.',1)
        tree+=ext.encode()+b'\0'+folder.encode()+b'\0'+stem.encode()+b'\0'
        tree+=struct.pack('<IHHIIH',zlib.crc32(b),0,0x7fff,len(data),len(b),65535)+b'\0\0';data+=b
    tree+=b'\0';path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(struct.pack('<III',0x55aa1234,1,len(tree))+tree+data)

def prepare(content,root):
    content,root=Path(content),Path(root)
    vpk=content/'game/hlvr/pak01_dir.vpk'
    index=dict(entries(vpk));files={}
    for key,xml in [('panorama/layout/game_menus/child02.vxml_c',True),('panorama/styles/game_menus/game_menu_main.vcss_c',False)]:
        files[key]=patch_resource(read(vpk,index[key]),xml)
    # The stylesheet references a pressed-state image absent from the shared
    # content. Supply an alias from the owner's existing hover texture, keeping
    # both the compiled stylesheet and its resource dependencies intact.
    pressed='panorama/images/game_menu_ui/options_advanced_press_png.vtex_c'
    hover='panorama/images/game_menu_ui/options_advanced_hover_png.vtex_c'
    if pressed not in index and hover in index:
        files[pressed]=read(vpk,index[hover])
    write_vpk(root/'game/native_ui_compat/pak01_dir.vpk',files)
    gameinfo=root/'game/hlvr/gameinfo.gi';text=gameinfo.read_text()
    if 'Game\t\t\t\tnative_ui_compat' not in text:
        text,count=re.subn(r'(SearchPaths\s*\{)',r'\1\n\t\t\tGame\t\t\t\tnative_ui_compat',text,count=1)
        if count!=1:raise ValueError('Cannot locate Alyx search paths')
    if gameinfo.is_symlink():gameinfo.unlink()
    gameinfo.write_text(text)

if __name__ == '__main__':
    import argparse
    p=argparse.ArgumentParser(description='Generate native Panorama compatibility assets in a private overlay from your installed Alyx files.')
    p.add_argument('content',type=Path,help='Owned Alyx directory containing game/hlvr')
    p.add_argument('overlay',type=Path,help='Private overlay with game/hlvr/gameinfo.gi')
    a=p.parse_args()
    if a.content.resolve()==a.overlay.resolve():p.error('Use a private overlay, not the original content directory')
    prepare(a.content,a.overlay)
