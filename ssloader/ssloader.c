/* ssloader.dll - loads the client mods listed in dlls.txt into WoW.exe.
 *
 * WoW.exe (as scripts/build-client.mjs patches it) calls LoadLibraryA("ssloader.dll") once at
 * startup and then the export func1. A missing DLL is skipped there, so the client also runs
 * without this file; it only loses the mods.
 *
 * dlls.txt lives next to WoW.exe: one DLL per line, relative to the game folder or absolute.
 * Empty lines and lines starting with # or ; are ignored. After a DLL is loaded, its export
 * "Load" is called if it has one. Everything is logged to Logs\ssloader.log.
 *
 * After the list it keeps the end of a moved texture read buffer free (FixTextureReadBuffer).
 *
 * Build (32-bit, like the client): build.cmd
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS /* every buffer below is terminated by hand */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define MAX_LIST (256 * 1024)
#define PATH_CHARS (MAX_PATH * 2)

static HANDLE logFile = INVALID_HANDLE_VALUE;
static char list[MAX_LIST + 1];

static void Log(const char *prefix, const char *text, int len)
{
	char line[1200];
	int n;
	DWORD written;

	if (logFile == INVALID_HANDLE_VALUE)
		return;
	if (len < 0)
		len = (int)strlen(text);
	n = _snprintf(line, sizeof(line) - 2, "[ssloader] %s%.*s", prefix, len, text);
	if (n < 0)
		n = sizeof(line) - 2;
	line[n++] = '\r';
	line[n++] = '\n';
	WriteFile(logFile, line, n, &written, NULL);
}

static void LoadOne(const WCHAR *dir, const char *name, int len)
{
	WCHAR wide[MAX_PATH];
	WCHAR full[PATH_CHARS];
	HMODULE module;
	FARPROC load;
	int n;

	n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, len, wide, MAX_PATH - 1);
	if (n <= 0)
		n = MultiByteToWideChar(CP_ACP, 0, name, len, wide, MAX_PATH - 1);
	if (n <= 0) {
		Log("Unreadable line: ", name, len);
		return;
	}
	wide[n] = 0;

	if (wide[0] == L'\\' || wide[0] == L'/' || (n > 1 && wide[1] == L':'))
		wcsncpy(full, wide, PATH_CHARS - 1);
	else
		_snwprintf(full, PATH_CHARS - 1, L"%s\\%s", dir, wide);
	full[PATH_CHARS - 1] = 0;

	if (GetModuleHandleW(full)) {
		Log("Already loaded: ", name, len);
		return;
	}
	module = LoadLibraryW(full);
	if (!module) {
		char reason[MAX_PATH + 32];
		_snprintf(reason, sizeof(reason) - 1, "%.*s (error %lu)", len, name, GetLastError());
		reason[sizeof(reason) - 1] = 0;
		Log("Failed to load: ", reason, -1);
		return;
	}
	Log("Loaded: ", name, len);

	load = GetProcAddress(module, "Load");
	if (load)
		((void (*)(void))load)();
}

/* WoW.exe packs texture files back to back into one read buffer: 2 MB inside its own data, where
 * reading a little past the end is harmless. VanillaHelpers.dll (HD pack) moves it to a 32 MB
 * VirtualAlloc block in its DllMain and raises the limit to match, so nothing readable follows the
 * block any more. The BLP reader (0x5A83B0) always copies a full 0x494-byte header, and Turtle ships
 * smaller BLPs (patch-9: Interface\WorldMap\Redridge\LAKEEVERSTILL6.blp, 276 bytes): one placed in
 * the last 0x494 bytes reads into the next page, ERROR #132 at 0x005A83DA. Lowering the limit keeps
 * the last 4 KB free. The three limits and bases are the imm32 of mov eax/ebx and lea edx (5875). */
#define TEX_BASE_IN_EXE 0x00905BE4
#define TEX_USED 0x00B05D18
#define TEX_FREE_END 0x1000

static BYTE *const texLimit[3] = { (BYTE *)0x0044971E, (BYTE *)0x0044A437, (BYTE *)0x00448C38 };
static BYTE *const texBase[3] = { (BYTE *)0x0044972A, (BYTE *)0x0044A443, (BYTE *)0x00448CA6 };

static void FixTextureReadBuffer(void)
{
	char text[160];
	DWORD limit, base, wanted, old;
	int i;

	if (texLimit[0][-1] != 0xB8 || texLimit[1][-1] != 0xB8 || texLimit[2][-1] != 0xBB
	    || texBase[0][-2] != 0x8D || texBase[0][-1] != 0x92 || texBase[1][-2] != 0x8D
	    || texBase[1][-1] != 0x92 || texBase[2][-2] != 0x8D || texBase[2][-1] != 0x90) {
		Log("", "Texture read buffer: unknown WoW.exe, left alone.", -1);
		return;
	}
	limit = *(DWORD *)texLimit[0];
	base = *(DWORD *)texBase[0];
	for (i = 1; i < 3; i++) {
		if (*(DWORD *)texLimit[i] != limit || *(DWORD *)texBase[i] != base) {
			Log("", "Texture read buffer: the three places differ, left alone.", -1);
			return;
		}
	}
	if (base == TEX_BASE_IN_EXE) {
		Log("", "Texture read buffer: still in WoW.exe, nothing to do.", -1);
		return;
	}
	wanted = limit - TEX_FREE_END;
	if (limit <= TEX_FREE_END || *(DWORD *)TEX_USED > wanted) {
		_snprintf(text, sizeof(text) - 1, "Texture read buffer at 0x%08lX: limit 0x%lX, used 0x%lX, left alone.",
		          base, limit, *(DWORD *)TEX_USED);
		text[sizeof(text) - 1] = 0;
		Log("", text, -1);
		return;
	}
	for (i = 0; i < 3; i++) {
		if (!VirtualProtect(texLimit[i], 4, PAGE_EXECUTE_READWRITE, &old)) {
			_snprintf(text, sizeof(text) - 1, "Texture read buffer: cannot write 0x%08lX (error %lu).",
			          (DWORD)texLimit[i], GetLastError());
			text[sizeof(text) - 1] = 0;
			Log("", text, -1);
			return;
		}
		*(DWORD *)texLimit[i] = wanted;
		VirtualProtect(texLimit[i], 4, old, &old);
	}
	FlushInstructionCache(GetCurrentProcess(), NULL, 0);
	_snprintf(text, sizeof(text) - 1, "Texture read buffer at 0x%08lX: limit 0x%lX -> 0x%lX (last 4 KB stay free).",
	          base, limit, wanted);
	text[sizeof(text) - 1] = 0;
	Log("", text, -1);
}

void func1(void)
{
	WCHAR dir[MAX_PATH];
	WCHAR path[PATH_CHARS] = {0}; /* _snwprintf below never writes the last element */
	WCHAR *slash;
	HANDLE file;
	DWORD got = 0;
	char *p;

	if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
		return;
	slash = wcsrchr(dir, L'\\');
	if (slash)
		*slash = 0;

	_snwprintf(path, PATH_CHARS - 1, L"%s\\Logs", dir);
	path[PATH_CHARS - 1] = 0;
	CreateDirectoryW(path, NULL);
	_snwprintf(path, PATH_CHARS - 1, L"%s\\Logs\\ssloader.log", dir);
	logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	Log("", "Log started.", -1);

	_snwprintf(path, PATH_CHARS - 1, L"%s\\dlls.txt", dir);
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		Log("", "dlls.txt not found.", -1);
	} else {
		ReadFile(file, list, MAX_LIST, &got, NULL);
		CloseHandle(file);
		list[got] = 0;

		p = list;
		if (got >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0)
			p += 3;
		while (*p) {
			char *end = strchr(p, '\n');
			char *next = end ? end + 1 : p + strlen(p);
			char *a = p;
			char *b = end ? end : next;

			while (a < b && (*a == ' ' || *a == '\t' || *a == '\r'))
				a++;
			while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r'))
				b--;
			if (b > a && *a != '#' && *a != ';')
				LoadOne(dir, a, (int)(b - a));
			p = next;
		}
	}
	FixTextureReadBuffer();

	if (logFile != INVALID_HANDLE_VALUE) {
		CloseHandle(logFile);
		logFile = INVALID_HANDLE_VALUE;
	}
}
