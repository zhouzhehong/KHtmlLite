# KHtmlLite installer

The ready-to-run installer is committed at:

`release/KHtmlLite-Setup-1.0.0.exe`

It is a per-user installer built with Inno Setup 6. It installs under
`%LOCALAPPDATA%\Programs\KHtmlLite`, creates a Start Menu shortcut, and
unconditionally creates the desktop shortcut `KHtmlLite.lnk` targeting
`khtml_browser.exe`.

To rebuild, first prepare the tested portable runtime in
`release/KHtmlLite-portable`, then run:

```powershell
"C:\Users\zhouzhehong\AppData\Local\Programs\InnoSetup6\ISCC.exe" installer\KHtmlLite.iss
```

The installer source is `installer/KHtmlLite.iss`.
