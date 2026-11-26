/* The watch app's C interface to the voice turn and the watch's end of the relay (voice_turn.h, relay.h), for Swift.
 * One client per app. Every string returned is malloc'd and freed with nw_free; every call is from one thread (the
 * app's main thread). Time is the caller's: milliseconds of one clock that never goes back. */

#ifndef NEXUS_WATCH_H
#define NEXUS_WATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nw_client nw_client;

/* The phases, as voice_turn.h's Phase. */
enum {
    NW_IDLE = 0, NW_LISTENING, NW_TRANSCRIBING, NW_CHOOSING, NW_SENDING, NW_WAITING, NW_SPEAKING, NW_INTERRUPTED,
    NW_DISCONNECTED
};
/* What a say is, as Say::Kind. */
enum { NW_CUE = 0, NW_NOTICE = 1, NW_REPLY = 2 };

/* A client for the watch `watch_id`, from its saved outbox (nw_saved), or none (saved NULL). Saved bytes that do not
 * read start an empty outbox and say so (a notice): never silently. */
nw_client* nw_client_new(const char* watch_id, const char* saved, size_t saved_len);
void nw_client_free(nw_client* c);
void nw_free(void* p);

/* The talk button. A release with audio of `format` (its file type) sends it to the phone's recognizer. Each returns
 * 1 when taken, 0 when not (with a notice when the person needs to know why). */
int nw_press(nw_client* c, uint64_t now_ms, int mic_allowed);
int nw_release(nw_client* c, uint64_t now_ms, const void* audio, size_t audio_len, const char* format);
/* The reply being read has been read; resume or drop an interrupted one. */
int nw_speech_done(nw_client* c);
int nw_resume(nw_client* c);
int nw_dismiss(nw_client* c);
int nw_tick(nw_client* c, uint64_t now_ms);
/* The owner's check: the watch unlocked and on the wrist since its passcode (LocalAuthentication's wrist-detection
   policy), or not. Locked, the watch neither listens nor reads a reply, and says so. A new client starts locked. */
void nw_lock(nw_client* c);
void nw_unlock(nw_client* c);
int nw_unlocked(const nw_client* c);

/* The carrier: the frame to send the phone now (its length; *out malloc'd), the phone's answer to it, or why there
 * was none (the phone unreachable, the relay not answering). nw_take returns 0 for an answer that does not read, and
 * says so in a notice. */
size_t nw_frame(nw_client* c, char** out);
int nw_take(nw_client* c, const char* bytes, size_t len);
void nw_carrier_failed(nw_client* c, const char* why);

/* What to show. */
int nw_phase(const nw_client* c);
const char* nw_phase_name(int phase);           /* static: not freed */
char* nw_current(const nw_client* c);           /* the AI talked to; "" for none */
size_t nw_ai_count(const nw_client* c);
char* nw_ai(const nw_client* c, size_t i);
char* nw_why(const nw_client* c);               /* Disconnected: why */
char* nw_words(const nw_client* c);             /* the words heard last */
size_t nw_outbox(const nw_client* c);           /* prompts not yet applied */
/* The reply being read while the phase is Speaking (or the one cut off, Interrupted): 1 and its text and AI, or
 * 0. The strings are malloc'd. */
int nw_reply(const nw_client* c, int* kind, char** text, char** ai_name);
/* The next cue or notice to say and show: 1 and it (taken out), or 0 for none. */
int nw_next_say(nw_client* c, int* kind, char** text);
/* What the screen shows (screen.h): its status line and its problem line, both malloc'd. The app passes what it
 * keeps itself: the last notice, why the owner's check refused ("" for none) and the AI whose reply is read. A fresh
 * launch settles at once: until the server is reached or screen.h's kSettleMs pass, a disconnected turn is drawn as
 * the idle screen. */
void nw_screen(const nw_client* c, const char* notice, const char* lock_why, const char* reply_from, char** status,
               char** problem);
/* The bytes to keep across launches: the id, the outbox and the next prompt's number. */
size_t nw_saved(const nw_client* c, char** out);

#ifdef __cplusplus
}
#endif

#endif
