# MKW100 runtime assets

Files the installer copies into `play/mkw100_assets/`, an overlay root the base game mounts over the disc
(`runtime/src/hle/hud_speedometer.cpp` registers it).

- `MKW100/MKW100Speedo.szs`: the speedometer layout (`game_image/PULSpeedo.brlyt`, `.brlan`, `.brctr`) and its
  digit pictures (`som_d_*.tpl`, `speed.tpl`), taken unchanged from `PulsarPackCreator/Resources/RaceAssets.szs`
  of [Pulsar](https://github.com/MelgMKW/Pulsar) by MelgMKW and repacked on their own
  (`RaceAssets.szs` SHA-1 fd9a1e33c5e428d54dc9f895633f93038b5082cc).

## Pulsar licence

MIT License

Copyright (c) 2023 MelgMKW

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
