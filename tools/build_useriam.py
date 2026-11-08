"""Build the selected 4.0 software backend plus its required 4.1 ABI adapter.

No older experimental adapter is generated. Frozen adapter/driver source is used.
Donor link libraries and upstream source must already be prepared; see BUILDING.
"""
from pathlib import Path
import json
import os
import subprocess
import sys

ROOT=Path(__file__).resolve().parent.parent
B=ROOT/'compat/useriam'
DEPS=Path(os.environ.get('DIPPER_DEPS','/root/hmos-work/graphics-deps'))
S=Path(os.environ.get('DIPPER_UPSTREAM',str(DEPS/'source')))
SDK=Path(os.environ.get('DIPPER_SDK','/root/hmos-work/sdk-6.0.0.47/installed/native'))
LIB=Path(os.environ.get('DIPPER_DONOR_LINK',str(ROOT/'work/donor-hdi-link')))
VENDOR=Path(os.environ.get('DIPPER_USERIAM_VENDOR_LIBS',str(B/'donor/vendor/lib64')))
HOST=Path(os.environ.get('DIPPER_USERIAM_HOST_LIBS',str(B/'donor/system/lib64/chipset-sdk-sp')))
OPENSSL=Path(os.environ.get('DIPPER_OPENSSL_INCLUDE',str(B/'openssl-include')))
env=os.environ.copy()
env.update(ANYHARMONYOS_OHOS_SOURCE=str(S),ANYHARMONYOS_SDK=str(SDK),
    ANYHARMONYOS_ROM_SYSTEM_LIBS=str(LIB),ANYHARMONYOS_ROM_VENDOR_LIBS=str(VENDOR),
    ANYHARMONYOS_OPENSSL_INCLUDE=str(OPENSSL))
if not OPENSSL.is_dir():raise SystemExit('Prepare target-compatible OpenSSL headers; DIPPER_OPENSSL_INCLUDE is missing.')
subprocess.run([sys.executable,str(B/'build_backend.py')],env=env,check=True)
O=B/'out'
commands=json.loads((O/'build-commands.json').read_text())
base=commands[-1]
flags=base[1:base.index('-shared')]
flags += ['-I'+str(B/'generated41'),'-I'+str(B/'generated41/user_auth'),'-Wno-unused-parameter']
clang=str(SDK/'llvm/bin/clang++')
backend=O/'libuser_auth_interface_service_4.0.z.so'
(O/'libdipper_userauth_4_0.so').write_bytes(backend.read_bytes())
subprocess.run([clang,*flags,'-shared','-Wl,-z,defs',
    '-Wl,-soname,libuser_auth_interface_service_4.1.z.so',str(B/'adapter41.cpp'),
    str(LIB/'libc++.so'),str(LIB/'libutils.z.so'),'-ldl','-o',
    str(O/'libuser_auth_interface_service_4.1.z.so')],check=True)
hdf=S/'drivers_hdf_core'
flags += ['-I'+str(p) for p in sorted({p.parent for p in (hdf/'interfaces/inner_api').rglob('*.h')})
          if 'khdf' not in str(p)]
for name in ['hdf_device_desc.h','hdf_sbuf_ipc.h','hdf_object_manager.h','hdf_device_section.h']:
    flags += ['-I'+str(p) for p in sorted({p.parent for p in hdf.rglob(name)})]
libs=[str(LIB/n) for n in ['libc++.so','libutils.z.so','libhilog.so','libhdf_utils.z.so',
    'libhdi.z.so','libipc_single.z.so']]
libs += [str(VENDOR/n) for n in ['libuser_auth_stub_4.0.z.so','libuser_auth_stub_4.1.z.so']]
libs += [str(HOST/n) for n in ['libhdf_host.z.so','libhdf_ipc_adapter.z.so']]
subprocess.run([clang,*flags,'-D__USER__','-shared','-Wl,-z,defs',
    '-Wl,-soname,libdipper_user_auth_driver.so',str(B/'driver41.cpp'),
    str(O/'libuser_auth_interface_service_4.1.z.so'),*libs,'-ldl',
    '-Wl,--allow-shlib-undefined','-o',str(O/'libdipper_user_auth_driver.so')],check=True)
