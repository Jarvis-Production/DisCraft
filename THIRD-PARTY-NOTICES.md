# Third-party notices

DisCraft is MIT-licensed (see `LICENSE`). A release also contains, or is built from, the following.

## Based on

| Component | License | Source |
|---|---|---|
| SkyCraft (the Fabric mod in `fabric/`, the shared-memory protocol, the link, input table, launcher and test stand-in are derived from it) | MIT | https://github.com/chasmlol/SkyCraft |

## In the Dishonored half (`Binaries/Win32/`)

| Component | License | Source |
|---|---|---|
| Ultimate ASI Loader v9.7.4 (unmodified, shipped as `d3d9.dll`; loads `DisCraft.asi`) | MIT | https://github.com/ThirteenAG/Ultimate-ASI-Loader |

`DisCraft.asi` itself uses only the Windows API, Direct3D 9 and DirectInput 8 from Windows, and
the C++ standard library of its compiler (MinGW-w64's libstdc++ statically, or Microsoft's).

## In the bundled Minecraft (`DisCraft-Minecraft.zip`)

| Component | License | Source |
|---|---|---|
| Prism Launcher 11.1.1 (unmodified portable Windows build) | GPL-3.0 | https://github.com/PrismLauncher/PrismLauncher |
| Fabric API 0.161.0+26.3 | Apache-2.0 | https://github.com/FabricMC/fabric |
| e4mc 6.2.2 | MIT | https://github.com/vgskye/e4mc-minecraft-architectury |

The bundle carries Prism Launcher's full license text as `Prism/LICENSE-PrismLauncher.txt`.

## Not included

Minecraft, Java and Fabric Loader aren't included. Prism Launcher downloads them from Mojang,
the Java vendor and FabricMC after the player signs in with a Microsoft account that owns
Minecraft: Java Edition. Dishonored isn't included either.

DisCraft isn't affiliated with or endorsed by Mojang, Microsoft, Arkane Studios, Bethesda or ZeniMax.
