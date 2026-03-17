# GW2 Story Times for Nexus

GW2 Story Times brings [gw2storytimes.com](https://gw2storytimes.com) into Guild Wars 2 through Raidcore Nexus. Browse community mission estimates, keep a compact timer on screen while you play, and submit your own runs back to the site without leaving the game.

## Features

- Compact always-on timer widget
- Pop-out mission browser with live data from GW2 Story Times
- Full Experience and Speedrun estimate views
- Stopwatch with pacing feedback based on community averages
- In-game time submissions tagged with `source: nexus`

## Installation

1. Install Raidcore Nexus in your Guild Wars 2 game folder.
2. Download the latest `gw2storytimes.dll` from the [Releases](https://github.com/MattyGroch/gw2-storytimes-nexus/releases) page.
3. Place the DLL in your Nexus addon folder.
4. Launch Guild Wars 2 and load the addon through Nexus.

## Using the Addon

### Timer Widget

The main Story Times widget is meant to stay on screen while you play. It shows:

- The currently selected mission
- The mission breadcrumb
- Your current run time
- The selected community estimate
- Basic pacing feedback

Use the widget buttons to:

- `Start` or `Pause` the timer
- `Reset` the current run
- `Clear` the selected mission
- `Submit` a completed run after stopping the timer

The default toggle hotkey is `ALT+SHIFT+T`.

### Mission Browser

Click `Browse Missions` on the widget to open the larger mission browser.

From there you can:

- Browse seasons in story order
- Search missions by mission, story, or season name
- Switch between Full Experience and Speedrun estimates
- Select a mission and load it into the timer widget

### Submitting Times

After stopping a run between 1 and 480 minutes, the `Submit` button becomes available.

You can submit your run as:

- `Full Experience`
- `Speedrun`

Submission data is sent to the GW2 Story Times API and tagged as coming from Nexus.

## Notes

- This addon uses live data from `api.gw2storytimes.com`
- Some BlishHUD-era features are still planned for the Nexus port
- If a submission fails, the widget will show the API error message directly

## Source Reference

The original BlishHUD module is available here:

- [https://github.com/MattyGroch/gw2-storytimerwidget](https://github.com/MattyGroch/gw2-storytimerwidget)
