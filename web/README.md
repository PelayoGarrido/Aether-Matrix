# Aether Matrix website

A static, responsive site with a draggable XY matrix, keyboard sliders, an optional Web Audio sketch, and installation notes. No frontend packages, accounts, analytics or cloud inference. The sound sketch illustrates interaction; it is not the native DSP engine.

```sh
cd web
pnpm dev
pnpm check
```

Serve `public/` on a static host. Put the verified macOS installer in `public/downloads/Aether_Matrix_macOS.pkg` before deployment; binaries are excluded from source control. The Pages workflow obtains both installers from successful native builds before publishing. Keep the build-status and host-validation wording accurate when updating release links.

Animation pauses off screen and when the tab is hidden. Reduced-motion preferences disable animation. Audio requires a click, stops when the tab is hidden, and never accesses the microphone. All assets are local.
