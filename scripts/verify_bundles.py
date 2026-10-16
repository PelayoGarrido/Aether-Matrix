#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path
root = Path(sys.argv[1])
for suffix, bundle_name in [('AU','Aether Matrix.component'),('VST3','Aether Matrix.vst3'),('Standalone','Aether Matrix.app')]:
    bundle = root / suffix / bundle_name
    subprocess.run(['codesign','--verify','--deep','--strict',str(bundle)],check=True)
    binary = bundle / 'Contents/MacOS/Aether Matrix'
    architectures = subprocess.check_output(['lipo','-archs',str(binary)],text=True).strip()
    if 'arm64' not in architectures or 'x86_64' not in architectures:
        raise SystemExit(f'Plugin is not universal: {bundle}: {architectures}')
    linked = subprocess.check_output(['otool','-L',str(binary)],text=True)
    if '_deps/' in linked or '.build-downloads/' in linked or '/Users/' in linked:
        raise SystemExit(f'Development dependency path leaked into {bundle}')
    if not (bundle/'Contents/Frameworks/libonnxruntime.dylib').exists():
        raise SystemExit(f'Bundled runtime missing: {bundle}')
    if not (bundle/'Contents/Resources/rave_decoder.onnx').exists():
        raise SystemExit(f'Default model missing: {bundle}')
    print(bundle_name, architectures, 'signature and bundle resources verified')
