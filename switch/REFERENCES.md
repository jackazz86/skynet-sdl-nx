# Reference ports studied (Android so-loader wrappers by NaGaa95, MIT)

lswtcs_nx, angrybirds2_nx, KHUx_nx, gtasa_nx, laytonbmr_nx (github.com/NaGaa95/<name>), plus
sts2_nx (ChanseyIsTheBest, the NativeAOT template). Read-only reference; nothing copied beyond
so_util (MIT, credited in CREDITS.md).

Useful ideas found, and what we did with them:

| Idea | Where | Status here |
|------|-------|-------------|
| bionic pthread objects reinterpreted: first word = pointer to a real newlib object, lazily created | lswtcs_nx/source/pthr.c | same design in shim.c (slot_of: 8-aligned slot, because bionic mutexes are 4-aligned) |
| deadman watchdog: if rendering stalls, crash on purpose so Atmosphere dumps every thread | lswtcs_nx/source/main.c | not yet; main loop now logs a 10 s heartbeat instead (mallinfo) |
| fake fds for pipe()/eventfd signalling | angrybirds2_nx/source/fakefd.c | our pipe() returns ENOSYS; use this if the runtime needs it |
| wrap `__libnx_exit` to skip destructors that crash on exit | gtasa_nx, lswtcs_nx (saltynx_exit.c) | not needed yet |
| SaltyNX FPS/frame-pacing block | */saltynx.cpp | optional later (frame pacing) |
| heap sizing: refuse/warn when the game gets far less than it needs (applet mode) | lswtcs_nx main.c | we take all RAM; applet mode prints total in the [mem] line |
| fuller fake JNI (strings, exceptions, many classes) | angrybirds2_nx/source/jni_fake.c (70 KB) | reference if more Java classes are needed |

Gyro: SkyNET's "head tracking" is for the phone stereo mode, not aiming, so the
"no sensors" log line is harmless. Gyro aiming would be a new feature (native SixAxis).
