/* sparkle.c - quest objects sparkle like a lootable corpse. Part of ssloader.dll.
 *
 * The server sets bit 0x08 of GAMEOBJECT_DYN_FLAGS on a chest, goober or generic object that this
 * player needs for a quest right now (core: GO_DYNFLAG_LO_SPARKLE). WoW 1.12 has no use for that bit,
 * so this module gives such an object the client's own lootable-corpse sparkle: the effect that
 * CGUnit_C adds at 0x6005D0 when a corpse becomes lootable.
 *
 * How (WoW.exe 1.12.1 build 5875 as Turtle ships it; every address is checked before use):
 *  - The tick runs after CGPlayer_C::Update of the active player (vtable slot 0x80AFB0, once a frame).
 *  - Every 250 ms the tick walks the visible objects (0x468380) and gives each flagged object that has
 *    no sparkle yet one: SMemAlloc(0x90), effect constructor 0x61F490, the loot-sparkle setup 0x61FA10
 *    (model from the client's own table, model-loaded callback 0x600640). The effect then gets flag 2
 *    (placed in the world at its own position, 0x620BE0) instead of the corpse's attachment point 19,
 *    which an object model does not have, and the object's position.
 *  - Every tick it updates its effects (0x620580; the first update after the model has loaded places
 *    it) and removes the effect of an object that lost the bit or is gone.
 *  - The effects hang in a list of this module (a stand-in owner), never in the object: 1.12's game
 *    object code does not clean an effect list. The game object's destructor (vtable slot 0, patched on
 *    first use) removes the object's effect before the object goes, so no effect outlives its object
 *    or its map.
 *  - Removing is stop (0x6203E0: model callbacks off), unlink (0x6203A0), release (0x6210E0: the last
 *    reference removes the world entry and the model and frees the effect).
 *
 * Test switches (environment): SS_SPARKLE_FLAG=<hex> watches another bit (1 = the "activate" bit, which a GM
 * gets on every quest object); SS_SPARKLE_LOG=1 logs every sparkle that comes and goes. Without them the log
 * gets the install line, the hooked destructor, the loot model and faults only.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS /* every buffer below is terminated by hand */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sparkle.h"

#define OBJ_MGR             0x00B41414u /* the object manager (s_curMgr) */
#define MGR_ACTIVE_GUID     0xC0        /* its active player GUID */
#define ENUM_VISIBLE        0x00468380u
#define OBJECT_PTR          0x00468460u
#define SMEM_ALLOC          0x006462E0u
#define EFFECT_CTOR         0x0061F490u
#define EFFECT_INIT_LOOT    0x0061FA10u
#define EFFECT_UNLINK       0x006203A0u
#define EFFECT_STOP         0x006203E0u
#define EFFECT_UPDATE       0x00620580u
#define EFFECT_RELEASE      0x006210E0u
#define MODEL_LOADED_CB     0x00600640u
#define UNIT_ADD_SPARKLE    0x006005D0u /* not called: its bytes prove the effect layout used here */
#define LOOT_ART_INDEX      0x00C4DF40u /* index of "HARDCODED Loot Art" in the effect name table, read by 0x61FA10 */
#define EFFECT_NAMES        0x00C0D760u /* the effect name records by index ... */
#define EFFECT_NAMES_MAX    0x00C0D764u /* ... and the highest index */
#define PLAYER_UPDATE_SLOT  0x0080AFB0u
#define PLAYER_UPDATE       0x005EC950u
#define TEXT_START          0x00401000u
#define TEXT_END            0x007FF000u
#define RDATA_START         0x007FF000u
#define RDATA_END           0x00827000u

#define TYPEMASK_GAMEOBJECT 0x20u

/* descriptor fields of a game object (dword index) */
#define F_GUID       0
#define F_POS_X      15
#define F_DYN_FLAGS  19
#define F_GO_TYPE    21

/* game object types that may sparkle: chest, generic, goober */
#define GO_TYPE_CHEST    3
#define GO_TYPE_GENERIC  5
#define GO_TYPE_GOOBER   10

/* effect fields (byte offset) */
#define E_GUID   0x08
#define E_ATTACH 0x24
#define E_FLAGS  0x2C
#define E_POS    0x48
#define E_NEXT   0x8C
#define EFLAG_WORLD 0x02u /* placed in the world at E_POS */
#define EFLAG_PLACED 0x800u /* set by 0x620BE0 once the model is loaded and placed */

/* the stand-in owner: 0x61FA10 reads the GUID through +0x08, creates the effect's model through the owner's model
 * at +0xD8 (in that model's scene) and links the effect into +0xB4 */
#define OWNER_GUID_PTR 0x08
#define OWNER_LIST     0xB4
#define OWNER_MODEL    0xD8
#define OBJECT_MODEL   0xD8 /* an object's model (CGObject_C), read by 0x61FA10 and 0x620BE0 */

#define SCAN_MS       250
#define MAX_SPARKLES  128
#define MAX_VTABLES   4
#define MAX_LOG_LINES 400

typedef void *(__stdcall *SMemAlloc_t)(DWORD size, const char *file, int line, DWORD flags);
typedef void *(__fastcall *ObjectPtr_t)(DWORD typeMask, const char *file, DWORD lo, DWORD hi, int line);
typedef int (__fastcall *EnumCb_t)(void *ctx, void *edx, DWORD lo, DWORD hi);
typedef int (__fastcall *EnumVisible_t)(EnumCb_t cb, void *ctx);
typedef void *(__fastcall *EffectCtor_t)(void *self, void *edx);
typedef void (__fastcall *EffectInit_t)(void *self, void *edx, void *owner, DWORD modelLoadedCb);
typedef void (__fastcall *EffectCall_t)(void *self, void *edx);
typedef void (__fastcall *PlayerUpdate_t)(void *self, void *edx, DWORD dt);
typedef void *(__fastcall *Destructor_t)(void *self, void *edx, DWORD flags);

static const struct {
	DWORD addr;
	int len;
	const char *bytes;
} signatures[] = {
	{ ENUM_VISIBLE, 12, "\xA1\x14\x14\xB4\x00\x53\x8B\x98\xAC\x00\x00\x00" },
	{ OBJECT_PTR, 14, "\x55\x8B\xEC\x8B\x45\x0C\x56\x8B\xF1\x8B\x4D\x08\x50\x51" },
	{ SMEM_ALLOC, 16, "\x55\x8B\xEC\x8B\x45\x10\x8B\x4D\x0C\x56\x8B\x75\x08\x50\x51\x56" },
	{ EFFECT_CTOR, 16, "\x8B\xC1\x33\xC9\x89\x08\x89\x48\x04\x89\x48\x08\x89\x48\x0C\x89" },
	{ EFFECT_INIT_LOOT, 16, "\x55\x8B\xEC\x53\x8B\x5D\x08\x8B\x43\x08\x56\x8B\xF1\x8B\x08\x89" },
	{ EFFECT_UNLINK, 16, "\x8B\x81\x88\x00\x00\x00\x33\xD2\x3B\xC2\x56\x74\x08\x8B\xB1\x8C" },
	{ EFFECT_STOP, 9, "\x56\x57\x68\xE4\xDE\xC4\x00\x8B\xF1" },
	{ EFFECT_UPDATE, 16, "\x55\x8B\xEC\x81\xEC\x04\x01\x00\x00\x53\x8B\xD9\x8B\x43\x2C\xF6" },
	{ EFFECT_RELEASE, 11, "\x56\x8B\xF1\xFF\x8E\x84\x00\x00\x00\x75\x14" },
	{ MODEL_LOADED_CB, 16, "\x55\x8B\xEC\x8B\x45\x0C\x85\xC0\x75\x25\x8B\x45\x14\x85\xC0\x74" },
	{ UNIT_ADD_SPARKLE, 16, "\x57\x8B\xF9\x8B\x87\x10\x01\x00\x00\xF6\x80\x24\x02\x00\x00\x01" },
};

static const SMemAlloc_t pSMemAlloc = (SMemAlloc_t)SMEM_ALLOC;
static const ObjectPtr_t pObjectPtr = (ObjectPtr_t)OBJECT_PTR;
static const EnumVisible_t pEnumVisible = (EnumVisible_t)ENUM_VISIBLE;
static const EffectCtor_t pEffectCtor = (EffectCtor_t)EFFECT_CTOR;
static const EffectInit_t pEffectInitLoot = (EffectInit_t)EFFECT_INIT_LOOT;
static const EffectCall_t pEffectUnlink = (EffectCall_t)EFFECT_UNLINK;
static const EffectCall_t pEffectStop = (EffectCall_t)EFFECT_STOP;
static const EffectCall_t pEffectUpdate = (EffectCall_t)EFFECT_UPDATE;
static const EffectCall_t pEffectRelease = (EffectCall_t)EFFECT_RELEASE;

static volatile LONG on;
static DWORD watchBit = 0x08;
static int verbose;
static DWORD lastScan;
static int count;
static DWORD owner[0x40];   /* stand-in owner, 0x100 bytes: its fields reach +0xD8 */
static DWORD ownerGuid[2];
static PlayerUpdate_t origPlayerUpdate;
static DWORD *vtables[MAX_VTABLES];
static Destructor_t origDestructor[MAX_VTABLES];
static int vtableCount;
static WCHAR logPath[MAX_PATH * 2];
static int logLines;
static int faults;
static DWORD faultCode, faultAt, faultData;

static int Fault(const EXCEPTION_POINTERS *ep)
{
	faultCode = ep->ExceptionRecord->ExceptionCode;
	faultAt = (DWORD)ep->ExceptionRecord->ExceptionAddress;
	faultData = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
	return EXCEPTION_EXECUTE_HANDLER;
}

/* ---- log: appends to Logs\ssloader.log after ssloader has closed it ---- */

static void RunLog(const char *format, ...)
{
	char line[400];
	va_list args;
	HANDLE file;
	DWORD written;
	SYSTEMTIME now;
	int n;
	int m;

	if (!logPath[0] || logLines >= MAX_LOG_LINES)
		return;
	logLines++;
	GetLocalTime(&now);
	n = _snprintf(line, 40, "[sparkle] %02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
	if (n < 0)
		n = 0;
	if (logLines == MAX_LOG_LINES) {
		m = _snprintf(line + n, sizeof(line) - 3 - n, "(log full, no more lines)");
	} else {
		va_start(args, format);
		m = _vsnprintf(line + n, sizeof(line) - 3 - n, format, args);
		va_end(args);
	}
	n += (m < 0) ? (int)sizeof(line) - 3 - n : m;
	line[n++] = '\r';
	line[n++] = '\n';
	file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
	                   FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	WriteFile(file, line, n, &written, NULL);
	CloseHandle(file);
}

/* ---- client data ---- */

static DWORD *Descriptor(const BYTE *object)
{
	return *(DWORD **)(object + 8);
}

static BYTE *GameObject(DWORD lo, DWORD hi)
{
	return (BYTE *)pObjectPtr(TYPEMASK_GAMEOBJECT, "sparkle.c", lo, hi, __LINE__);
}

static BYTE *FirstEffect(void)
{
	return *(BYTE **)((BYTE *)owner + OWNER_LIST);
}

static BYTE *FindEffect(DWORD lo, DWORD hi)
{
	BYTE *e;

	for (e = FirstEffect(); e; e = *(BYTE **)(e + E_NEXT))
		if (*(DWORD *)(e + E_GUID) == lo && *(DWORD *)(e + E_GUID + 4) == hi)
			return e;
	return NULL;
}

/* stop, unlink, release: the last reference removes the world entry and the model and frees it */
static void DestroyEffect(BYTE *e)
{
	pEffectStop(e, NULL);
	pEffectUnlink(e, NULL);
	pEffectRelease(e, NULL);
	count--;
}

static void RemoveSparkle(DWORD lo, DWORD hi, const char *why)
{
	BYTE *e = FindEffect(lo, hi);

	if (!e)
		return;
	DestroyEffect(e);
	if (verbose)
		RunLog("off %08lX%08lX (%s), %d left", hi, lo, why, count);
}

/* 0x61FA10 takes the model from the record of "HARDCODED Loot Art"; it does not check that one is there */
static const char *LootArtModel(void)
{
	static int said;
	int index = *(int *)LOOT_ART_INDEX;
	int max = *(int *)EFFECT_NAMES_MAX;
	DWORD *records = *(DWORD **)EFFECT_NAMES;
	const DWORD *record = (index >= 0 && index <= max && records) ? (const DWORD *)records[index] : NULL;
	const char *model = record ? (const char *)record[2] : NULL;

	if (!said) {
		said = 1;
		RunLog("loot art: index %d of %d, record 0x%08lX, model %s", index, max, (DWORD)record,
		       model ? model : "(none)");
	}
	return model;
}

static void AddSparkle(const BYTE *go, DWORD lo, DWORD hi, const DWORD *desc)
{
	DWORD model = *(const DWORD *)(go + OBJECT_MODEL);
	BYTE *e;

	if (!model || !LootArtModel())
		return; /* no model yet: the next scan tries again */
	e = (BYTE *)pSMemAlloc(0x90, "sparkle.c", __LINE__, 0);
	if (!e)
		return;
	pEffectCtor(e, NULL);
	ownerGuid[0] = lo;
	ownerGuid[1] = hi;
	*(DWORD *)((BYTE *)owner + OWNER_MODEL) = model;
	pEffectInitLoot(e, NULL, owner, MODEL_LOADED_CB);
	*(DWORD *)((BYTE *)owner + OWNER_MODEL) = 0;
	if (FirstEffect() != e) {
		/* 0x61FA10 found no model, destroyed and freed the effect itself */
		RunLog("no sparkle model for %08lX%08lX", hi, lo);
		return;
	}
	*(DWORD *)(e + E_FLAGS) |= EFLAG_WORLD;
	*(DWORD *)(e + E_ATTACH) = 0xFFFFFFFFu;
	memcpy(e + E_POS, &desc[F_POS_X], 3 * sizeof(DWORD));
	pEffectRelease(e, NULL); /* the list keeps the one reference, as 0x600638 does */
	count++;
	if (verbose)
		RunLog("on %08lX%08lX entry %lu type %lu at %.1f %.1f %.1f, %d on", hi, lo, desc[3], desc[F_GO_TYPE],
		       ((const float *)desc)[F_POS_X], ((const float *)desc)[F_POS_X + 1],
		       ((const float *)desc)[F_POS_X + 2], count);
}

/* ---- game object destructor: its sparkle goes first ---- */

static void *__fastcall DestructorHook(void *self, void *edx, DWORD flags)
{
	DWORD *vt = *(DWORD **)self;
	Destructor_t orig = origDestructor[0];
	int i;

	for (i = 0; i < vtableCount; i++) {
		if (vtables[i] == vt) {
			orig = origDestructor[i];
			break;
		}
	}
	if (FirstEffect()) {
		__try {
			DWORD *desc = Descriptor((BYTE *)self);
			if (desc)
				RemoveSparkle(desc[F_GUID], desc[F_GUID + 1], "object gone");
		} __except (Fault(GetExceptionInformation())) {
			InterlockedExchange(&on, 0);
			if (faults++ < 3)
				RunLog("fault 0x%08lX at 0x%08lX (address 0x%08lX) in the destructor hook, sparkle off", faultCode,
				       faultAt, faultData);
		}
	}
	return orig(self, edx, flags);
}

static int HookDestructor(const BYTE *object)
{
	DWORD *vt = *(DWORD **)object;
	DWORD old;
	int i;

	for (i = 0; i < vtableCount; i++)
		if (vtables[i] == vt)
			return 1;
	if (vtableCount == MAX_VTABLES || (DWORD)vt < RDATA_START || (DWORD)vt >= RDATA_END
	    || vt[0] < TEXT_START || vt[0] >= TEXT_END)
		return 0;
	if (!VirtualProtect(vt, sizeof(DWORD), PAGE_READWRITE, &old))
		return 0;
	origDestructor[vtableCount] = (Destructor_t)vt[0];
	vtables[vtableCount] = vt;
	vtableCount++;
	vt[0] = (DWORD)DestructorHook;
	VirtualProtect(vt, sizeof(DWORD), old, &old);
	RunLog("game object destructor 0x%08lX hooked (vtable 0x%08lX)", (DWORD)origDestructor[vtableCount - 1],
	       (DWORD)vt);
	return 1;
}

/* ---- the tick ---- */

static int __fastcall ScanOne(void *ctx, void *edx, DWORD lo, DWORD hi)
{
	BYTE *go = GameObject(lo, hi);
	DWORD *desc;
	DWORD type;

	(void)ctx;
	(void)edx;
	if (!go)
		return 1;
	desc = Descriptor(go);
	if (!desc || !(desc[F_DYN_FLAGS] & watchBit))
		return 1;
	type = desc[F_GO_TYPE];
	if (type != GO_TYPE_CHEST && type != GO_TYPE_GENERIC && type != GO_TYPE_GOOBER)
		return 1;
	if (count >= MAX_SPARKLES || FindEffect(lo, hi))
		return 1;
	if (!HookDestructor(go))
		return 1;
	AddSparkle(go, lo, hi, desc);
	return 1;
}

static void Tick(void)
{
	BYTE *e = FirstEffect();
	BYTE *next;
	DWORD now = GetTickCount();

	for (; e; e = next) {
		DWORD lo = *(DWORD *)(e + E_GUID);
		DWORD hi = *(DWORD *)(e + E_GUID + 4);
		BYTE *go = GameObject(lo, hi);

		next = *(BYTE **)(e + E_NEXT);
		if (!go || !(Descriptor(go)[F_DYN_FLAGS] & watchBit)) {
			DestroyEffect(e);
			if (verbose)
				RunLog("off %08lX%08lX (%s), %d left", hi, lo, go ? "bit cleared" : "not found", count);
			continue;
		}
		if (verbose && !(*(DWORD *)(e + E_FLAGS) & EFLAG_PLACED)) {
			pEffectUpdate(e, NULL);
			if (*(DWORD *)(e + E_FLAGS) & EFLAG_PLACED)
				RunLog("placed %08lX%08lX", hi, lo);
			continue;
		}
		pEffectUpdate(e, NULL);
	}
	if (now - lastScan >= SCAN_MS) {
		lastScan = now;
		pEnumVisible(ScanOne, NULL);
	}
}

static void __fastcall PlayerUpdateHook(void *self, void *edx, DWORD dt)
{
	BYTE *mgr;
	DWORD *desc;

	origPlayerUpdate(self, edx, dt);
	if (!on)
		return;
	mgr = *(BYTE **)OBJ_MGR;
	desc = Descriptor((BYTE *)self);
	if (!mgr || !desc || desc[F_GUID] != *(DWORD *)(mgr + MGR_ACTIVE_GUID)
	    || desc[F_GUID + 1] != *(DWORD *)(mgr + MGR_ACTIVE_GUID + 4))
		return;
	__try {
		Tick();
	} __except (Fault(GetExceptionInformation())) {
		InterlockedExchange(&on, 0);
		if (faults++ < 3)
			RunLog("fault 0x%08lX at 0x%08lX (address 0x%08lX) in the tick, sparkle off", faultCode, faultAt,
			       faultData);
	}
}

/* ---- install ---- */

void SparkleInstall(void (*log)(const char *text))
{
	char text[200];
	char value[16];
	WCHAR dir[MAX_PATH];
	WCHAR *slash;
	DWORD old;
	size_t i;

	for (i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++) {
		if (memcmp((const void *)signatures[i].addr, signatures[i].bytes, signatures[i].len) != 0) {
			_snprintf(text, sizeof(text) - 1, "Sparkle: unknown WoW.exe at 0x%08lX, off.", signatures[i].addr);
			text[sizeof(text) - 1] = 0;
			log(text);
			return;
		}
	}
	if (*(DWORD *)PLAYER_UPDATE_SLOT != PLAYER_UPDATE) {
		_snprintf(text, sizeof(text) - 1, "Sparkle: CGPlayer_C::Update slot holds 0x%08lX, off.",
		          *(DWORD *)PLAYER_UPDATE_SLOT);
		text[sizeof(text) - 1] = 0;
		log(text);
		return;
	}
	if (GetEnvironmentVariableA("SS_SPARKLE_FLAG", value, sizeof(value)) > 0) {
		DWORD bit = strtoul(value, NULL, 16);
		if (bit)
			watchBit = bit;
	}
	if (GetEnvironmentVariableA("SS_SPARKLE_LOG", value, sizeof(value)) > 0)
		verbose = value[0] == '1';
	if (GetModuleFileNameW(NULL, dir, MAX_PATH)) {
		slash = wcsrchr(dir, L'\\');
		if (slash)
			*slash = 0;
		_snwprintf(logPath, sizeof(logPath) / sizeof(logPath[0]) - 1, L"%s\\Logs\\ssloader.log", dir);
	}
	*(DWORD **)((BYTE *)owner + OWNER_GUID_PTR) = ownerGuid;

	if (!VirtualProtect((void *)PLAYER_UPDATE_SLOT, sizeof(DWORD), PAGE_READWRITE, &old)) {
		_snprintf(text, sizeof(text) - 1, "Sparkle: cannot write 0x%08lX (error %lu), off.", PLAYER_UPDATE_SLOT,
		          GetLastError());
		text[sizeof(text) - 1] = 0;
		log(text);
		return;
	}
	origPlayerUpdate = (PlayerUpdate_t)(*(DWORD *)PLAYER_UPDATE_SLOT);
	*(DWORD *)PLAYER_UPDATE_SLOT = (DWORD)PlayerUpdateHook;
	VirtualProtect((void *)PLAYER_UPDATE_SLOT, sizeof(DWORD), old, &old);
	on = 1;
	_snprintf(text, sizeof(text) - 1, "Sparkle: on, objects with bit 0x%02lX of GAMEOBJECT_DYN_FLAGS sparkle.",
	          watchBit);
	text[sizeof(text) - 1] = 0;
	log(text);
}
