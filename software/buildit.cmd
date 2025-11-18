@ECHO OFF
rmdir /s /q build
mkdir build
cd build
cmake -G "MinGW Makefiles" -DPICO_COPY_TO_RAM=1 ..
make -j4
copy apps\dmg\dmg.uf2 e:\