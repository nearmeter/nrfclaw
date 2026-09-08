#ifndef NRFCLAW_TRACKING_JOURNAL_H
#define NRFCLAW_TRACKING_JOURNAL_H

#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_TRACKING_JOURNAL_A_ADDR 0x00076000UL
#define NRFCLAW_TRACKING_JOURNAL_B_ADDR 0x00077000UL

typedef enum
{
    NRFCLAW_TRACKING_JOURNAL_EMPTY = 0,
    NRFCLAW_TRACKING_JOURNAL_READY,
    NRFCLAW_TRACKING_JOURNAL_BUSY,
    NRFCLAW_TRACKING_JOURNAL_ERROR
} nrfclaw_tracking_journal_status_t;

typedef struct
{
    uint32_t index;
    uint32_t epoch;
} nrfclaw_tracking_journal_entry_t;

void nrfclaw_tracking_journal_init(void);
void nrfclaw_tracking_journal_process(void);

nrfclaw_tracking_journal_status_t
nrfclaw_tracking_journal_status(void);

bool nrfclaw_tracking_journal_latest(
    nrfclaw_tracking_journal_entry_t *entry);

/*
 * Atomically append a new monotonically increasing index.
 *
 * The caller MUST NOT advertise the key for `index` until the journal status
 * returns READY and latest().index == index.
 */
bool nrfclaw_tracking_journal_append(
    uint32_t index,
    uint32_t epoch);

#endif
