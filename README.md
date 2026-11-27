<img src="assets/icon-256.png" alt="XR Window Panels" width="96" align="right">

# XR Window Panels

An OpenXR API layer that shows desktop windows as floating panels inside Direct3D 11 VR apps and games (iRacing, ACC, LMU, AMS2, DesktopXR...). It was made to bring telemetry and coaching overlays into VR, such as the throttle and brake traces of a reference lap.

> 🇧🇷 [Versão em português](README.pt-BR.md)

## Install

1. Download the latest zip from [Releases](../../releases) and extract it to a folder.
2. Double-click **`Install.cmd`** in File Explorer and accept the administrator prompt.

- The layer is registered machine-wide (HKLM).
- The control app is copied over and gets a desktop shortcut: **XR Window Panels**.

Run the installer from File Explorer or a normal PowerShell, never from inside a packaged (MSIX) app. Windows redirects those apps' writes to a private copy, and games won't see the layer.

To remove it, use **`Uninstall.cmd`**. Your `config.ini` is kept.

## Control app (`XRWindowPanelsControl.exe`)

- **Panels**: add one panel per window.
- **Window**: pick the window from the list. It is matched by "Title contains" and "Process". For windows whose title changes (such as Chrome tabs), clear the title and keep only the process.
- **Position**: these buttons can be held down to repeat.
  - Recenter (this panel) and Recenter all
  - Move ▲◀▶▼
  - Bigger/Smaller
  - Nearer/Further
  - Tilt/turn
  - "Step" switches between fine, normal and coarse adjustment.
- **Appearance**: opacity, chroma key (remove a background color), the window's own transparency.
- **Updates/s**: how many times per second the panel image is refreshed. Lower is lighter.
- The footer shows whether a VR game is connected, and each panel's status in VR.

The app is in English, or in Portuguese when Windows is set to Portuguese. To force a language, set the `XRWP_LANG` environment variable to `en` or `pt`.

Changes show up in VR instantly. They are saved in `%LOCALAPPDATA%\XRWindowPanels\config.ini`.

- **Remember the recentered position** (option in the Position group, off by default): when on, recentering a panel makes the layer save that position in `anchors.ini` and restore it the next time you start the game. When off, recentering only lasts while the game is running.
  - The position is stored in the runtime's STAGE space, which is fixed to the room. That way the panel comes back to the same spot in the cockpit even if the runtime recenters the view.
  - If the runtime has no STAGE space, the layer uses LOCAL space.
  - If you redo the room setup on your headset, recenter again.
- **Start with Windows**: tick the option in the app's footer. The app then starts with Windows, minimized to the tray, so shortcuts work from the start.
  - Opening the app again (from the shortcut) brings up the window that is already running instead of a second copy.

## In-game shortcuts

Built-in layer shortcuts (they work even without the app open):

- `Ctrl+F12`: show/hide the panels. While hidden, they use no GPU at all.
- `Ctrl+F11`: recenter all panels in front of your head.

### Custom shortcuts: keyboard, wheel and button box

In the control app, click **⌨ Shortcuts...** and double-click an action. Then press the key (Ctrl, Shift or Alt combos work) or the button on your wheel, button box or joystick.

- **Available actions**:
  - Recenter (selected / all)
  - Show/hide all
  - Next/previous panel
  - Enable/disable panel
  - Move ▲◀▶▼, Bigger/Smaller, Nearer/Further, Tilt/turn. These moves repeat while the button is held.
- **Which panel moves**: move actions apply to the selected panel. When you switch panels, it gets a **yellow border for 1.5 s in VR**, so you know which one is selected.
- **Controllers**: wheels, button boxes and joysticks are read through Raw Input (HID), with no button limit (Simagic bases, for example, expose more than 100), including the D-pad (POV/hat). The capture dialog shows live what each controller is sending.
- **App open**: shortcuts work while the game has focus, as long as the app is open. Minimized, it goes to the tray and keeps working.
- **Where they are saved**: `%LOCALAPPDATA%\XRWindowPanels\bindings.ini`.

## Performance

The goal is for the layer to cost the game almost nothing:

- **Off the game's render thread**: finding the window, starting the capture and reading the config run on separate low-priority threads.
- **Capture separate from the game**: it uses its own graphics device (with lowered GPU priority) and hands the image over through a shared texture. Nothing waits: if the resource is busy, it tries again next frame.
- **Work only on change**: the GPU only works when the window changes, and at most at the configured updates/s (30 by default). On other frames, the only cost is resubmitting the panel.
- **Game state untouched**: the game's Direct3D state is saved and restored only on frames that have something to draw.

## Log

`%LOCALAPPDATA%\XRWindowPanels\layer.log`

## Build

Requires Visual Studio 2022 Build Tools (C++), CMake and git. The control app uses the .NET Framework `csc.exe` that ships with Windows.

```powershell
cmake -S . -B <short-folder>\build -G "Visual Studio 17 2022" -A x64
cmake --build <short-folder>\build --config Release
```

Use a build folder with a short path: MSBuild fails on paths longer than 260 characters.

Test tool that works without VR:

```powershell
capture_test --list
capture_test --config
capture_test "Google Chrome" output.bmp
```

## Limitations

- Direct3D 11 games only.
- You can't interact with the panels using VR controllers.
