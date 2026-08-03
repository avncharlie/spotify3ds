#pragma once

/*
 * Headless test + logging harness.
 *
 * svcOutputDebugString is compiled out of Azahar release builds, so the only
 * reliable way to get text off the guest is to write files to the SD card:
 *   emulator -> ~/Library/Application Support/Azahar/sdmc/
 *   hardware -> the real SD card
 *
 * Both channels flush after every write so a crash still leaves a readable
 * trail. An unflushed buffer lost on crash is the difference between a
 * diagnosable failure and a mystery.
 */

/* Open sdmc:/testresult.txt (truncating) and sdmc:/spotify/log.txt (appending). */
void tl_init(int phase);

/* Free-form debug line -> log.txt only. printf-style. */
void tl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Environment fact, emitted once at startup as "BANNER <key>=<value> ...".
 * Makes every run self-describing, so a transcript alone explains what it ran
 * on - including the build stamp, which is what stops us debugging a stale
 * .3dsx. */
void tl_banner(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Machine-readable verdict -> BOTH testresult.txt and log.txt.
 * Emits: PHASE=<n> STEP=<step> RESULT=PASS|FAIL detail=<...> */
void tl_step(const char *step, int pass, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Write the DONE sentinel and close. Absence of DONE in testresult.txt means
 * the app hung or crashed, which is distinct from a clean FAIL. */
void tl_done(void);
