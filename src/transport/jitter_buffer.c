#include "vmc/transport/jitter_buffer.h"

#include <string.h>

#include "vmc/core/error.h"

vmc_status vmc_jb_init(vmc_jitter_buffer *jb, u64 target_delay_us) {
    if (!jb) return VMC_ERR_INVALID_ARG;
    memset(jb, 0, sizeof(*jb));
    jb->target_delay_us = target_delay_us;
    return VMC_OK;
}

void vmc_jb_reset(vmc_jitter_buffer *jb, u32 first_seq) {
    memset(jb, 0, sizeof(*jb));
    jb->next_seq = first_seq;
}

/* Wrapped seq comparison helper (handles u32 wraparound). */
static inline i32 seq_cmp(u32 a, u32 b) {
    return (i32)(a - b);
}

static vmc_jb_slot *find_slot(vmc_jitter_buffer *jb, u32 seq) {
    for (int i = 0; i < VMC_JB_MAX_SLOTS; i++) {
        if (jb->slots[i].valid && jb->slots[i].seq == seq) {
            return &jb->slots[i];
        }
    }
    return NULL;
}

vmc_status vmc_jb_push(vmc_jitter_buffer *jb, u32 seq, u32 ts_us,
                       u8 stream, u16 flags, const u8 *payload, u16 len,
                       u64 recv_us) {
    if (!jb || (len > 0 && !payload)) return VMC_ERR_INVALID_ARG;

    if (jb->stats_pushed == 0) {
        /* First push establishes the playout base. */
        jb->next_seq = seq;
    }

    /* Duplicate: already buffered. */
    if (find_slot(jb, seq)) {
        jb->stats_dropped_dupe++;
        return VMC_ERR_AGAIN;
    }

    /* Outside the reorder window: too far ahead or too far behind. */
    const i32 fwd = seq_cmp(seq, jb->next_seq);
    if (fwd >= VMC_JB_MAX_SLOTS || fwd <= -VMC_JB_MAX_SLOTS) {
        jb->stats_dropped_late++;
        return VMC_ERR_OUT_OF_SYNC;
    }

    vmc_jb_slot *slot = NULL;
    for (int i = 0; i < VMC_JB_MAX_SLOTS; i++) {
        if (!jb->slots[i].valid) { slot = &jb->slots[i]; break; }
    }
    if (!slot) {
        jb->stats_dropped_late++;
        return VMC_ERR_OVERRUN;
    }

    slot->seq     = seq;
    slot->ts_us   = ts_us;
    slot->flags   = flags;
    slot->stream  = stream;
    slot->len     = len;
    slot->recv_us = recv_us;
    slot->valid   = true;
    if (len > 0) {
        /* Copy into owned storage: the caller's receive buffer is reused. */
        const sz_t cap = len < VMC_JB_PAYLOAD_CAP ? len : VMC_JB_PAYLOAD_CAP;
        memcpy(slot->storage, payload, cap);
        slot->len = (u16)cap;
    }
    slot->payload = slot->storage;
    jb->stats_pushed++;
    return VMC_OK;
}

const vmc_jb_slot *vmc_jb_peek(vmc_jitter_buffer *jb, u64 now_us) {
    if (!jb) return NULL;

    vmc_jb_slot *min = NULL;
    vmc_jb_slot *newest = NULL;
    for (int i = 0; i < VMC_JB_MAX_SLOTS; i++) {
        vmc_jb_slot *s = &jb->slots[i];
        if (!s->valid) continue;
        if (!min || seq_cmp(s->seq, min->seq) < 0) { min = s; }
        if (!newest || s->recv_us > newest->recv_us) { newest = s; }
    }
    if (!min) return NULL;

    for (int iter = 0; iter < 2; iter++) {
        if (seq_cmp(min->seq, jb->next_seq) < 0) {
            /* Late-but-windowed arrival: play it once old enough. */
            if (now_us - min->recv_us >= jb->target_delay_us) {
                jb->next_seq = min->seq;
                return min;
            }
            return NULL;
        }
        if (seq_cmp(min->seq, jb->next_seq) > 0) {
            /* Gap: next_seq is missing. Declare it lost once the newest
             * arrival has been held for the target delay. */
            if (now_us - newest->recv_us >= jb->target_delay_us) {
                jb->stats_gaps += (u64)(min->seq - jb->next_seq);
                jb->next_seq = min->seq;
                if (now_us - min->recv_us >= jb->target_delay_us) {
                    return min;
                }
            }
            return NULL;
        }
        /* min->seq == next_seq: play when old enough. */
        if (now_us - min->recv_us >= jb->target_delay_us) {
            return min;
        }
        return NULL;
    }
    return NULL;
}

void vmc_jb_consume(vmc_jitter_buffer *jb) {
    vmc_jb_slot *slot = find_slot(jb, jb->next_seq);
    if (!slot) return;
    slot->valid = false;
    jb->next_seq++;
    jb->stats_played++;
}

void vmc_jb_flush_before(vmc_jitter_buffer *jb, u32 seq) {
    for (int i = 0; i < VMC_JB_MAX_SLOTS; i++) {
        if (jb->slots[i].valid && seq_cmp(jb->slots[i].seq, seq) < 0) {
            jb->slots[i].valid = false;
        }
    }
    if (seq_cmp(seq, jb->next_seq) > 0) {
        jb->next_seq = seq;
    }
}

u64 vmc_jb_estimate_jitter_us(const vmc_jitter_buffer *jb) {
    (void)jb;
    /* Full EWMA tracking added in a later iteration. */
    return 0;
}
