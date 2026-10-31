# KHtmlLite installer

The ready-to-run installer is published only as a GitHub Releases asset:

https://github.com/zhouzhehong/KHtmlLite/releases/tag/v1.0.0

It is intentionally **not stored in the repository file tree**. The local
build output `release/KHtmlLite-Setup-1.0.0.exe` is ignored by Git and should
be uploaded to the matching GitHub Release after rebuilding.

It is a per-user installer built with Inno Setup 6. It installs under
`%LOCALAPPDATA%\Programs\KHtmlLite`, creates a Start Menu shortcut, and
unconditionally creates the desktop shortcut `KHtmlLite.lnk` targeting
`khtml_browser.exe`.

To rebuild, first prepare the tested portable runtime in
`release/KHtmlLite-portable`, then run:

```powershell
"C:\Users\zhouzhehong\AppData\Local\Programs\InnoSetup6\ISCC.exe" installer\KHtmlLite.iss
```

Upload the resulting installer to the GitHub Release; do not commit it to the
repository tree. The installer source is `installer/KHtmlLite.iss`.
