# ssloader

`ssloader.dll` loads client mods into the World of Warcraft 1.12.1 client (build 5875). It is part of the
SoloShell client.

## How the client loads it

The SoloShell client's WoW.exe calls `LoadLibraryA("ssloader.dll")` once at startup and then the DLL's export
`func1`. Without the file the client still runs, only without mods.

## dlls.txt

`dlls.txt` lies next to WoW.exe: one DLL per line, relative to the game folder or absolute. Empty lines and lines
that start with `#` or `;` are skipped. After a DLL is loaded, its export `Load` is called if it has one. Everything
is logged to `Logs\ssloader.log`.

## What else it does

- **Texture read buffer.** VanillaHelpers.dll moves the client's texture read buffer to a 32 MB block with nothing
  readable behind it, and the client's BLP reader always copies a full 0x494-byte header. A smaller BLP at the very
  end of the block then reads past it (ERROR #132 at 0x005A83DA). After the list, ssloader lowers the limit so that
  the last 4 KB stay free (`FixTextureReadBuffer` in `ssloader.c`).
- **Quest objects sparkle** (`sparkle.c`). A chest, goober or generic object that the player needs for a quest gets
  the client's own lootable-corpse sparkle. The server marks such objects with bit 0x08 of `GAMEOBJECT_DYN_FLAGS`;
  with a server that does not set it, nothing changes.

## Build

Windows with Visual Studio 2022 (C++ workload): run `ssloader\build.cmd`. It writes `ssloader.dll` (32-bit, like
WoW.exe) into this folder. `/Brepro` and no debug information: the same source gives the same bytes, and no build
path ends up in the DLL. Built from this source, `ssloader.dll` has the SHA-1
`8709f949dc11af7962dc4a73f352ae3e371f1f30`.

## License

MIT, see [LICENSE](LICENSE).
