# Aether Matrix website

A static, responsive product site using the real native instrument capture, licensed studio photography and a locally hosted H.264 film. The audio example is an actual four-bar hybrid-engine export. No frontend packages, accounts, analytics or cloud inference.

```sh
cd web
pnpm dev
pnpm check
```

Serve `public/` on a static host. The Pages workflow obtains the macOS and Windows installers from successful native builds of the same commit before publishing. Binaries are excluded from source control. Keep preview and host-validation wording accurate when updating release links.

Motion uses native browser animations: staged typography, scroll-controlled product perspective, section reveals and animated close views. Reduced-motion disables automatic film playback and transitions. The film pauses off screen and when the tab is hidden; a visible control can pause it manually. Data-saver disables automatic video loading. Audio requires a click and pauses when the tab is hidden. No microphone access is requested. All runtime assets are local; the CSP blocks external requests.

See [ASSETS.md](ASSETS.md) for media creators, source URLs, licensing and audio settings. Third-party media retain their own licences. The actual plugin screenshot is used for all product details; there are no generated SVG illustrations or fictional product renders.
