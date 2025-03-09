# Crank 'N Arc

arc emulator for Playdate and norns

## Building

```
mkdir build_device
cd build_device
cmake \
 -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake \
 -DCMAKE_BUILD_TYPE=Release \
 ..
```

Crank 'N Arc.pdx can now be sideloaded to your Playdate.
