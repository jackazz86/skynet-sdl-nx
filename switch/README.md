# switch/ — Nintendo Switch homebrew port (so-loader)

Personal-use only. Loads `libSkyNet.so` (NativeAOT, android-arm64) from the official
SkyNET APK on Horizon via a minimal bionic/Android shim, and maps SDL3 onto a native
Switch SDL3. No game binaries or data are, or may be, committed here.

Nothing outside this folder is modified, so upstream syncs stay conflict-free.
See RUN_NOTES.md for the log of every test run. Loader/shim design follows
sts2_nx (MIT, Andy Nguyen, fgsfds); see CREDITS.md.
