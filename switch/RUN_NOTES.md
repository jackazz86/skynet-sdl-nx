# RUN_NOTES

Una riga per prova: data, modifica, risultato.

| Data | Modifica | Risultato |
|------|----------|-----------|
| 2026-11-26 | Fase 0: APK 1.6.1 scaricato, SHA-256 di libSkyNet.so e libSDL3.so coincidono con il piano. 284 simboli indefiniti, 10349 RELATIVE / 282 JUMP_SLOT / 3 GLOB_DAT, EH_FRAME presente, 1874 stringhe SDL_. | OK (analisi). Build .nro non ancora possibile: devkitPro non raggiungibile da questo ambiente. |
| 2026-11-26 | Fase 1a: APK 1.7.1 (libSkyNet.so sha256 706574a6…, libSDL3.so a8c679e1…). 284 simboli indefiniti (stessi nomi della 1.6.1 + @LIBC). Aggiunti switch/source (so_util da sts2_nx, imports generati: 59 passthrough, il resto stub che loggano), main.c che carica/rilocca/risolve e stampa gli export (SDL_main, JNI_OnLoad). | Compila (devkitA64, libnx) → skynet_nx.nro 328 KB. NON ancora provato su Switch. |
| 2026-11-26 | Prima prova su Switch (skynet_sdl_nx.nro, libSkyNet.so 1.7.1 da FTP). | OK: so_load (load_size=43020 KB), nessun import non risolto stampato, export trovati: SDL_main 0x61e1…, JNI_OnLoad 0x61e5f7790, JavaInteropRuntime_init 0x61e5f77b20. Finalize/init_array non ancora eseguiti. |
| 2026-11-26 | Fase 1b: shim.c (TLS per thread, pthread lazy-object, clock bionic->HOS, stat/open/dirent/FILE/__sF, mmap su memalign + mprotect via svcSetMemoryPermission, locale/wide single-byte, dlopen/dlsym loggati, rete/processi = ENOSYS), env DOTNET_* (GC hard limit 256 MB, region range 320 MB, no concurrent GC), heap = tutta la RAM. main ora fa finalize + init_array (NON JNI_OnLoad/SDL_main). | Compila, .nro 475 KB caricato via FTP. In attesa del test su Switch. |
