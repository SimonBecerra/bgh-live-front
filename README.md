# BGH LIVE Front

The phone remote for the laptop at the front of the auditorium at Barrington Gospel Hall. It's an OBS plug-in:
once installed, OBS runs a small web page that phones on the church Wi-Fi open.

- **Presenter** phones change the slideshow on the projector (Back and Next), and show the slide that's on it.
- **Controller** phones switch OBS scenes (Translator: Spanish, Korean or English, Baptism Cam, Black), put OBS
  full screen on the OPTOMA projector, and start or stop recording.

## Download

**[Download BGH-LIVE-Front-Setup.exe](https://github.com/SimonBecerra/bgh-live-front/releases/latest/download/BGH-LIVE-Front-Setup.exe)**
(Windows, for OBS Studio 31 or later). This link always gives the newest version.

## Install

1. Close OBS on the front laptop.
2. Run **BGH-LIVE-Front-Setup.exe**. Windows may say it protected your PC, because the installer isn't signed
   with a paid certificate: click **More info**, then **Run anyway**. Choose **Yes** when Windows asks for permission.
3. Open OBS. A **BGH LIVE Front** panel appears with a QR code. If it doesn't, open **Tools → BGH LIVE Front**.
   Place the panel where you like; OBS remembers it.

## Pair a phone

1. Put the phone on the church Wi-Fi (the same network as the laptop).
2. Point its camera at the QR code in the BGH LIVE Front panel. It opens the remote and pairs straight away.
   Or open the address shown in the panel (like `http://192.168.1.20:8650`) and type the 3-digit code.
3. Choose **Presenter** or **Controller**. Tap the label at the top to switch later.
4. Optional: in Safari, Share → **Add to Home Screen**, so it opens like an app.

A paired phone stays paired. Remove one in the panel's **Paired phones** list and it stops working straight away.
The code changes after each phone pairs, and five wrong codes pause pairing for 5 minutes.

## How it works

- **Slides.** With **PowerPoint**, the remote tells PowerPoint itself to go to the next or previous slide, so it
  works even while OBS is projecting over the slideshow. The phone shows the slide, the next slide and "5 / 8".
  With anything else full screen on the OPTOMA (Google Slides or Keynote in a browser, a PDF), it presses the
  Left and Right arrow keys on that window, and the phone shows a picture of the projector screen.
- **Projector.** The cast button opens OBS's Program full screen on the screen with **OPTOMA** in its name, and
  never anywhere else. Pressing it again asks before closing it, and the slideshow shows again. The panel says
  whether the OPTOMA is connected and lists every screen's name. To use a different projector, change
  `kProjectorName` in `src/front.h`.
- **Scenes.** Only the five scenes above appear on the phone; other scenes in OBS are ignored. Each needs two taps.
  They're listed in `allowedScenes()` in `src/state.cpp`.
- **Network.** The installer allows TCP port 8650 through Windows Firewall. Only phones on the same network can reach it.

## Where things are on the laptop

- The plug-in: `C:\ProgramData\obs-studio\plugins\bgh-live-front`
- Paired phones: `%APPDATA%\obs-studio\plugin_config\bgh-live-front\phones.json`
- Problems: OBS → **Help → Log Files → View Current Log**, search for `bgh-live-front`. The log also lists every
  connected screen's name, which is where to find the projector's exact name.

## Building

GitHub builds the installer on every push to `main` (`.github/workflows/build.yml`) and publishes it as the newest
release. The plug-in uses OBS's plug-in template build scripts.
