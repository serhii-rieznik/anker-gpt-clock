# Anker Clock

A lightweight Windows tray app that puts a clock on the display of the
[Anker A210C desktop USB-C hub](https://www.anker.com/products/a210c-10-in-1-desktop-usb-c-hub-dual-4k-smart-display).

![Soft Frame analog clock with Netatmo readings and Codex usage](docs/images/analog.jpg)

![Digital clock on a gradient background](docs/images/digital.jpg)

## What it does

- Three analog clock faces and digital clocks with adjustable fonts, sizes, colors, and time zone.
- Browse fonts with clock previews and [compare them side by side](docs/images/font-comparison.png).
- Gradient backgrounds, your own images, or pure black. Gradient smoothing reduces visible banding.
- A configurable refresh interval, every 10 seconds by default.
- A lock badge and elapsed lock duration on the clock.
- Optional Netatmo readings and Codex usage, automatic start with Windows, and pause on battery.

![Settings window](docs/images/settings.png)

## Download and run

[Download the latest Windows release](https://github.com/serhii-rieznik/anker-gpt-clock/releases/latest).

Requires Windows 10 or 11 (x64) and .NET Framework 4.8.

1. Extract the ZIP, keep its files together, and connect the hub by USB.
2. Launch `AnkerClock.exe`.
3. Open settings from the tray icon, customize your clock, and click **Save changes**.

Anker Dock Manager is not required. Close it if it is running so the two apps
do not compete for the display. The hub fades between images.

The app uses the gallery only for the clock. Old images are removed without backups.

## Netatmo readings

In **Settings → Netatmo**, import a TXT or JSON file containing your Netatmo client ID,
client secret, access token and refresh token. Windows protects the stored credentials.
Choose a module, the readings to display and a refresh interval. Temperature, humidity,
CO₂ and noise are supported where available; the default display uses temperature,
CO₂ and noise.

Old readings stay visible with a small clock badge. Settings shows measurement age,
connection errors and refresh status.

## Codex usage

Install and sign in to the Codex CLI or desktop app, then enable **Show Codex usage**.
It shows your remaining allowance on the analog clock and lock screen, refreshing
once a minute by default. If your account has multiple limits, it shows the lowest
remaining percentage. Unavailable readings stay hidden.

## Build

Install Visual Studio 2022 with **Desktop development with C++** and a Windows SDK,
the **.NET Framework 4.8 targeting pack**, .NET SDK 8 or newer, and CMake 3.25 or newer.
From the source folder:

```powershell
cmake --preset windows-vs2022
cmake --build --preset release-vs2022
cmake --install build/vs2022 --config Release --prefix dist
```

The runnable app is in `dist/`.

## License

[MIT](LICENSE). This is an independent project, unaffiliated with Anker or OpenAI.
