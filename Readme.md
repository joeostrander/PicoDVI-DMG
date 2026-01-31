# PicoDVI-DMG  

## Output Gameboy DMG video and audio via HDMI

Also see the emulator version:  
https://github.com/joeostrander/PicoDVI-DMG_EMU 

--- 
Update 2026.01.31  
 - PCB Images and gerber  
 
Update 2026.01.20
 - Added the On-Screen Display

Update 2026.01.18
 - Moved audio to core1 (where libdvi runs)
 - Performance enhancements
 - Fixed glitches
 - Save preferences
 - New PCB coming soon

Update 2025.12.05
 - 2 resolution options available (see CMakeLists.txt) 
 - Selectable schemes with Select+left/right
 - Frameblending with Select+Home
 - PIO-based video capture

Update 2025.11.24
 - Audio is now working! 

---


![gameplay preview](./images/gameplay.gif?raw=true)  

![pcb rev 2](./images/pcb_rev2.png?raw=true)  

![dmg theme](./images/preview_dmg.jpg?raw=true)  

![gbp theme](./images/preview_gbp.jpg?raw=true)  

![osd](./images/osd.jpg?raw=true)  



I'll probably integrate it into:  
https://github.com/joeostrander/consolized-game-boy  

Special thanks to:  
PicoDVI - Wren6991  
PicoDVI-N64 - kbeckmann  
Andy West (element14)  





Building
--------

You need to have the Pico SDK installed, as well as a recent CMake and arm-none-eabi-gcc.


```bash
mkdir build
cd build
cmake -DPICO_SDK_PATH=/path/to/sdk -DPICO_PLATFORM=rp2040 -DPICO_COPY_TO_RAM=1 ..
make -j$(nproc)
```
Note:  on Windows I use:  
```bash
cmake -G "MinGW Makefiles" -DPICO_COPY_TO_RAM=1 ..
```
Thanks to Shawn Hymel for his awesome guides on rp2040 setup for VSCode and Windows

```bash
linux:  
cp apps/dmg/dmg.uf2 /media/${USER}/RPI-RP2/

windows:
copy apps\dmg\dmg.uf2 <driveletter>:
```
