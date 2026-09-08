# Quest 2: black world after the first cutscene

1. Finish updating and installing the current diagnostic build. Connect the Quest to Windows by USB, enable developer mode and accept the **USB debugging** prompt inside the headset.
2. Double-click **`ENABLE_RENDER_DIAGNOSTICS.bat`**. It enables basic render logging, turns pixel readbacks off, verifies both properties, and stops SAVR. Wait for its success message. Enabling diagnostics must happen **before** the game launch that you want to record.
3. Start SAVR yourself and reproduce the black screen. Keep the game open and the headset awake. Collect immediately, before restarting the game or headset.
4. Double-click **`COLLECT_RENDER_REPORT.bat`**. Keep the `tools` folder beside the BAT files. The tools use the kit's `.tools/platform-tools/adb.exe`, Android SDK, SideQuest, or adb from PATH.
5. Find the ZIP in **Desktop / SAVR Render Reports**. Review it, then **attach the complete ZIP manually** to your support reply. Nothing is uploaded automatically. Include your mod version, Quest model, what you did before the screen went black, whether menus/HUD and CJ movement still work, and whether Quest Games Optimizer was active.
6. After collecting, double-click **`DISABLE_RENDER_DIAGNOSTICS.bat`**. It disables both diagnostic properties and stops SAVR; manually launch the game again when ready to play.

`COLLECT_RENDER_REPORT.bat` reads existing data only. It does not launch or stop the game, change settings, install anything, clear logs, or upload files. It copies the game's `savr_debug.txt`, `savr_profile.csv` and `vr_*.ini` files when present, plus filtered graphics logs, recent crash logs and device/package information. It does not request saves, APKs or game assets. Logs can include paths, earlier runs and other apps' graphics/crash diagnostics; review them before sharing.

The ENABLE and DISABLE launchers are explicit setup actions. They change only `debug.savr.render_diag` and `debug.savr.render_diag_pixels`, verify the values, and stop only `com.rockstargames.gtasa`. They never launch or install the game, clear logs, or upload anything. Run ENABLE again after restarting the headset if another capture is needed. Running UPDATE and COLLECT alone does **not** activate the new logging channel.

Missing files are normal. Some older release builds compile out SAVR logging, so an empty report does not establish the cause or prove that rendering works. An existing original 0.2.0 installation can still provide device settings and driver/runtime errors using COLLECT alone; it cannot produce the new `[render.diag]` channel without the updated build. Collect a separate report for each version you test and label which version produced it. Installing another build is not part of these diagnostic tools.

For a custom adb location, multiple connected devices, or a custom output folder, run from the kit folder in PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\collect-render-report.ps1 -AdbPath "C:\Android\platform-tools\adb.exe" -Serial "YOUR_QUEST_SERIAL" -OutputDirectory "C:\SAVR Reports"
```

The script also supports PowerShell 7 (`pwsh`). Add `-EnableDiagnostics` or `-DisableDiagnostics` to the command above for setup; these two switches cannot be combined, and setup does not collect a report. The BAT launchers accept the same optional adb/device arguments. Omit `-Serial` when only one authorized device is connected. Find serials with `adb devices`; unauthorized/offline devices cannot be selected. A report with collection warnings is still useful: `README_REPORT.txt` and `commands.json` identify missing captures and adb errors. If the connection drops, reconnect and collect again. The ZIP and its reviewable folder are both retained on the PC.
