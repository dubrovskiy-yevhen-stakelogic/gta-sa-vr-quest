# Quest 2: black world after the first cutscene

1. Connect the Quest to Windows by USB. Enable developer mode and accept the **USB debugging** prompt inside the headset.
2. Start SAVR yourself and reproduce the black screen. Keep the game open and the headset awake. Collect immediately, before restarting the game or headset.
3. Double-click `COLLECT_RENDER_REPORT.bat` in the kit folder. Keep the `tools` folder beside it. The collector uses the kit's `.tools/platform-tools/adb.exe`, Android SDK, SideQuest, or adb from PATH.
4. Find the ZIP in **Desktop / SAVR Render Reports**. Review it, then send it to the mod developer with your mod version, Quest model, what you did before the screen went black, whether menus/HUD and CJ movement still work, and whether Quest Games Optimizer was active.

The script reads existing data only. It does not launch or stop the game, change settings, install anything, clear logs, or upload files. It copies the game's `savr_debug.txt`, `savr_profile.csv` and `vr_*.ini` files when present, plus filtered graphics logs, recent crash logs and device/package information. It does not request saves, APKs or game assets. Logs can include paths, earlier runs and other apps' graphics/crash diagnostics; review them before sharing.

Missing files are normal. Some older release builds compile out SAVR logging, so an empty report does not establish the cause or prove that rendering works. An existing 0.2.0 installation can still provide device settings and driver/runtime errors. The new `[render.diag]` channel requires an updated diagnostic build and separate developer instructions; this collector does not enable it. Collect a separate report for each version you test and label which version produced it. Installing another build is not part of this collector.

For a custom adb location, multiple connected devices, or a custom output folder, run from the kit folder in PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\collect-render-report.ps1 -AdbPath "C:\Android\platform-tools\adb.exe" -Serial "YOUR_QUEST_SERIAL" -OutputDirectory "C:\SAVR Reports"
```

The script also supports PowerShell 7 (`pwsh`). Omit `-Serial` when only one authorized device is connected. Find serials with `adb devices`; unauthorized/offline devices cannot be selected. A report with collection warnings is still useful: `README_REPORT.txt` and `commands.json` identify missing captures and adb errors. If the connection drops, reconnect and collect again. The ZIP and its reviewable folder are both retained on the PC.
