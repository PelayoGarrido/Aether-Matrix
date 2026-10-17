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
    helpers=bundle/'Contents/Helpers'
    if helpers.exists():
        for name in ('AetherMidiEvaluate','AetherPianistPerform','AetherPianoRender'):
            helper=helpers/name
            subprocess.run(['codesign','--verify','--strict',str(helper)],check=True)
            imports=subprocess.check_output(['otool','-L',str(helper)],text=True)
            load_commands=subprocess.check_output(['otool','-l',str(helper)],text=True)
            if any(path in imports+load_commands for path in ('/Users/','.build-downloads/','_deps/')):
                raise SystemExit('Development dependency path leaked into piano helper: '+name)
        for name in ('salamander-manifest.json','salamander-layout.json'):
            if not (bundle/'Contents/Resources/piano'/name).is_file(): raise SystemExit('Piano mapping missing: '+name)
        forbidden={'model.safetensors','model_base.onnx','model_token.onnx'}
        if any(path.name in forbidden or path.name=='Samples' for path in bundle.rglob('*')):
            raise SystemExit('Research weights or sample bank must not be bundled')
    print(bundle_name, architectures, 'signature, resources and optional native helpers verified')
