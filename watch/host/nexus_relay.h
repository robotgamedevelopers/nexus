/* The phone's end of the watch's relay, over nexus's own link (link/client.h), for Swift: the iPhone app's
 * WatchConnectivity delegate (watch/phone/WatchRelayPhone.swift) and the Mac's relay host that stands in for the phone
 * in the simulator's end-to-end test (watch/host/main.swift) call the same functions. Strings and bytes returned are
 * malloc'd and freed with nr_free. Any thread: calls are serialized inside. */

#ifndef NEXUS_RELAY_H
#define NEXUS_RELAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nr_host nr_host;

/* A relay to the nexus-server that `ssh <host>` reaches, running `command` there (NULL: "nexus-server new"; a test
 * names its own server's home and directory), keeping what must outlive the app in the file `state_path` (0600).
 * NULL, with *why (malloc'd), when the state file is there and does not read. */
nr_host* nr_host_new(const char* host, const char* const* command, size_t command_count, const char* state_path,
                     char** why);
void nr_host_free(nr_host* h);
void nr_free(void* p);

/* One frame from the watch: the answer to send back (its length; *reply malloc'd), and, when the frame brought a new
 * recording, *utterance set (0 for none) with the recording's bytes and file type for the recognizer. 0 and *why for
 * a frame that does not read, or a state file that cannot be written (nothing is then handed to the link). */
size_t nr_host_exchange(nr_host* h, const char* frame, size_t len, char** reply, uint64_t* utterance, char** audio,
                        size_t* audio_len, char** format, char** why);
/* The recognizer's words for a recording, or why it found none. */
void nr_host_transcribed(nr_host* h, uint64_t utterance, int ok, const char* text);
/* Moves what the link changed to the relay and the relay's prompts to the link: call often (every 100 ms). 0 with *why
 * when the state file cannot be written: nothing more is handed to the link until it can be. */
int nr_host_pump(nr_host* h, char** why);
/* The phone's own lock: passed since the phone last slept (1), or not (0). A new relay is locked: it refuses the
   watch, saying so, until this says 1. */
void nr_host_set_unlocked(nr_host* h, int unlocked);
/* The link's phase, as the watch is told (0 connecting, 1 connected, 2 lost). */
int nr_host_link(nr_host* h);

#ifdef __cplusplus
}
#endif

#endif
