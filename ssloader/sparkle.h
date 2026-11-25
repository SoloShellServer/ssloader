/* sparkle.h - quest objects sparkle like a lootable corpse (see sparkle.c). */
#ifndef SSLOADER_SPARKLE_H
#define SSLOADER_SPARKLE_H

/* Checks WoW.exe and hooks it in; every line it has to say goes through log. */
void SparkleInstall(void (*log)(const char *text));

#endif
