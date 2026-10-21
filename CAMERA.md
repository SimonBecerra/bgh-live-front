# BGH Front: OBSBOT Tiny 2 camera control

Prototype: **BGH Front Camera** artifact (https://claude.ai/artifact/YGVgt71zcdjPGGzSqdP9ez). It's built on BGH Front Remote and ready to merge into it.

## How it works
Phone (Controller mode) → church Wi-Fi → BGH LIVE Front plug-in in OBS → OSC over UDP to OBSBOT Center on 127.0.0.1 → Tiny 2 over USB.
OBSBOT Center must be running. It can sit minimized in the tray, and it doesn't need to be the window in front.

## Phone UI (Controller mode)
- A square camera button between Record and Swap. When the camera isn't plugged in, tapping it shows "Camera not detected".
- The camera sheet fits on one screen with no scrolling. From top to bottom:
  - "Live on stream" tag, shown while Baptism Cam is in Program.
  - Preview: still frames from the laptop about 4 times a second.
  - Arrows (up, down, left, right): hold one to move. It starts slowly and reaches full speed after about 1.5 s,
    so a tap nudges the camera. One direction at a time. Let go and it stops.
  - Zoom slider from 1.00× to 4.00×. Only the thumb drags; a tap on the track does nothing.
  - **Go to Shot** (switch-camera icon) recalls the preset named **Front** in OBSBOT Center. It is a double tap, and it is greyed out when OBSBOT Center has no preset called Front. There is no Set button: aim the camera, then use **Update PTZ** on the Front preset in OBSBOT Center.
  - **Done** at the bottom.
- There are no presets, no follow speaker, and no recenter button. The user decided against all three.

## Plug-in behaviour
- Starts OBSBOT Center (minimized) when OBS opens, if it isn't running. It can also be started from the phone.
- Each time OBSBOT Center connects: `GetDeviceInfo`, `SetAiMode 0`, `ToggleAILock 0` (tracking off).
- Wakes a sleeping camera (`WakeSleep 1`) when a phone opens Camera or when Baptism Cam goes live.
- Arrows: `SetGimbalLeft/Right/Up/Down` with speed 1–100. Speed 0 stops. Changing direction sends 0 to the old direction first.
- Deadman stop: the phone repeats a held push every 250 ms. If the repeats stop for 600 ms, the plug-in sends 0 by itself.
- Zoom: `SetZoom 0–100`, sent in steps of 4 or more, plus the final value.
- Presets: `/OBSBOT/WebCam/Tiny/GetPresetPositionInfo 0` returns `PresetPositionInfo` with (saved, name) for each of 3 slots. The plug-in finds the slot named Front (any case) at start-up, every 10 s, and just before each Go to Shot. OSC has no command to save or update a preset, so Update PTZ stays in OBSBOT Center.
- Go to Shot: `/OBSBOT/WebCam/Tiny/TriggerPreset <slot 0-2>`.
- One phone at a time: other phones are told who is moving the camera.
- Baptism Cam's camera is set to stay on when the scene isn't showing, so the preview works before going live.
- Warns on the phone and in the OBS panel when OBS gets no picture, which usually means OBSBOT Center's preview is holding the camera.
- OBS panel settings:
  - Start OBSBOT Center when OBS opens
  - Keep the laptop awake while OBS is open
  - Camera is mounted upside down (flips both directions)
  - Arrows top speed (30–100%)
  - Check directions (nudges left and back)

## Do by hand

### Test first, before building the plug-in
1. In OBSBOT Center, turn on OSC (UDP, address 127.0.0.1) and note the port. 16284 is assumed.
2. From TouchOSC or a script, confirm on the real Tiny 2:
   - `SetGimbalLeft` moves the camera.
   - `SetZoom` zooms.
   - `GetGimbalPosInfo` sends back numbers.
   - `SetGimMotorDegree` points the camera at an angle.
   - `WakeSleep` and `SetAiMode 0` behave as described.

   Go to Shot depends on `GetPresetPositionInfo` and `TriggerPreset`. Check OBS's log for `camera: the "Front" preset is slot …`.

### OBSBOT Center, once
3. Turn off gesture control and voice control. There is no OSC command for either.
4. Turn off its preview (the eye icon) so OBS gets the picture.
5. Turn off auto-sleep if it has the option.
6. Use "start minimized" or "start with Windows" if it offers them.
7. Note the install path, usually C:\Program Files\OBSBOT Center.

### Windows on the front laptop, once
8. Turn off USB selective suspend (Power Options, then Advanced settings).
9. If the laptop ever runs closed, set the lid-close action to "Do nothing".
10. Plug the camera in directly, or through a powered hub.

### Phones
11. Set auto-lock to 10 minutes or Never for services. The http page can't keep the screen on by itself.
12. Save the remote to the home screen.

### After the plug-in is built
13. Press Check directions. If the picture moves right first, tick "Camera is mounted upside down".
14. Tune the arrows' top speed on a test stream.
15. Check the Wi-Fi where the operator stands.

### Each service
16. Make sure the Front preset in OBSBOT Center is aimed where you want it before switching to Baptism Cam.
17. Decide who runs the camera.
18. If the shot needs to change, aim the camera and press Update PTZ on Front in OBSBOT Center. The preset is kept by OBSBOT Center, so it survives restarts.

## Status
- Merged into BGH Front Remote and the BGH LIVE Front plug-in (`src/camera.cpp`), from version 1.0.0.21.
- Not yet tried on the real Tiny 2: do "Test first" above. The plug-in writes each OSC reply address from
  OBSBOT Center to OBS's log once (search the log for `camera:`), which shows whether the position and zoom
  replies arrive and what they look like.
- The OBS panel's Camera section also has the OBSBOT Center folder, the OSC port (16284) and the reply port
  (16285, where OBSBOT Center sends its answers).
