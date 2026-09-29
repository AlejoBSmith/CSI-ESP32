Import("env")
import hashlib
import pathlib
import subprocess
root = pathlib.Path(env['PROJECT_DIR']).parent
try:
    revision = subprocess.check_output(['git', '-c', 'safe.directory='+str(root).replace('\\','/'), '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
except Exception:
    revision = 'unknown'
digest = hashlib.sha256()
for p in sorted((root/'firmware').rglob('*')):
    if p.is_file() and not any(x in p.parts for x in ('.pio','managed_components')) and p.name!='build_version.h' and p.suffix in ('.cpp','.h','.ini','.defaults','.py','.txt','.yml','.lock'):
        digest.update(str(p.relative_to(root)).replace('\\','/').encode()); digest.update(p.read_bytes())
content='#pragma once\n#define RAT_COMMIT "'+revision+'"\n#define RAT_SOURCE_HASH "'+digest.hexdigest()+'"\n'
header=root/'firmware'/'main'/'build_version.h'
if not header.exists() or header.read_text()!=content:header.write_text(content)
# PlatformIO's CMake bridge does not propagate this component's INTERFACE
# prebuilt archive. Link the pinned ESP-IDF 5.5 / ESP32-S3 library explicitly.
env.Append(LIBPATH=[str(root/'firmware'/'managed_components'/'espressif__esp_csi_gain_ctrl'/'5.5'/'esp32s3')], LIBS=['esp_csi_gain_ctrl'])
