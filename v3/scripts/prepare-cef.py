#!/usr/bin/env python3
"""Prepare the pinned CEF runtime and (on macOS) an ad-hoc signed helper bundle."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import shutil
import subprocess
import tarfile
import urllib.request

VERSION='154.0.28+g564dd6c+chromium-154.0.8037.58'
CHECKSUMS={
 'windows64':'34a32277db8addfa355a009ed16c8dae939daed1',
 'macosarm64':'f7c50c2719deb8aa0c78de517c1228d0b8c3fced',
 'macosx64':'60bb4e9d525d40a8ee721c55557a9edd058873dc',
}
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--platform',choices=CHECKSUMS,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--binary',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
name=f'cef_binary_{VERSION}_{a.platform}_minimal'
archive=a.output/(name+'.tar.bz2')
if not archive.exists():urllib.request.urlretrieve('https://cef-builds.spotifycdn.com/'+name+'.tar.bz2',archive)
with archive.open('rb') as f:digest=hashlib.file_digest(f,'sha1').hexdigest()
if digest!=CHECKSUMS[a.platform]:raise RuntimeError('CEF archive checksum mismatch')
with tarfile.open(archive) as t:t.extractall(a.output,filter='data')
source=a.output/name
runtime=a.output/'runtime';runtime.mkdir(exist_ok=True)
if a.platform=='windows64':
 for part in ['Release','Resources']:shutil.copytree(source/part,runtime,dirs_exist_ok=True)
 print(runtime)
else:
 app=a.output/'CEFSmoke.app';contents=app/'Contents';macos=contents/'MacOS';macos.mkdir(parents=True,exist_ok=True)
 shutil.copy2(a.binary,macos/'CEFSmoke')
 framework=contents/'Frameworks';framework.mkdir(exist_ok=True)
 shutil.copytree(source/'Release'/'Chromium Embedded Framework.framework',framework/'Chromium Embedded Framework.framework',symlinks=True,dirs_exist_ok=True)
 helper=framework/'CEFSmoke Helper.app'/'Contents';(helper/'MacOS').mkdir(parents=True,exist_ok=True)
 shutil.copy2(a.binary,helper/'MacOS'/'CEFSmoke Helper')
 for directory,executable,identifier,background in [(contents,'CEFSmoke','io.wails.cefsmoke',False),(helper,'CEFSmoke Helper','io.wails.cefsmoke.helper',True)]:
  with (directory/'Info.plist').open('wb') as f:
   plistlib.dump(dict(CFBundleExecutable=executable,CFBundleIdentifier=identifier,CFBundleName=executable,CFBundlePackageType='APPL',CFBundleVersion='1',NSHighResolutionCapable=True,LSUIElement=background),f)
 entitlements=a.output/'entitlements.plist'
 with entitlements.open('wb') as f:plistlib.dump({'com.apple.security.cs.allow-jit':True,'com.apple.security.cs.allow-unsigned-executable-memory':True,'com.apple.security.cs.disable-library-validation':True},f)
 subprocess.run(['codesign','--force','--deep','--sign','-','--entitlements',str(entitlements),str(app)],check=True)
 print(app)
