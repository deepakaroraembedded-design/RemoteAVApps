/*
 * state.h — thin client session lifecycle state machine.
 *
 * Mirrors the RTOS global state machine from the VMC whitepaper: offline
 * fallback engages automatically on network degradation; the device never
 * leaves the degraded state with an active session.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_SESSION_STATE_H
#define VMC_SESSION_STATE_H

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

typedef enum {
    VMC_SESSION_OFFLINE = 0,   /* boot / airplane mode / no network */
    VMC_SESSION_DISCOVER,      /* contacting mapper, awaiting container route */
    VMC_SESSION_CONNECTING,    /* opening media transport to container */
    VMC_SESSION_ACTIVE,        /* streaming nominal */
    VMC_SESSION_DEGRADED,      /* link impaired; reduced quality path active */
    VMC_SESSION_RECONNECTING,  /* transport lost; re-establishing */
    VMC_SESSION_TERMINATING,   /* user/logout/security termination */
    VMC_SESSION_COUNT
} vmc_session_state;

/* Event type feeding the state machine. */
typedef enum {
    VMC_EVENT_BOOT,             /* device powered / network up */
    VMC_EVENT_MAPPER_RESP,      /* mapper returned container route */
    VMC_EVENT_TRANSPORT_UP,     /* media transport established */
    VMC_EVENT_LINK_OK,          /* QoS/link quality restored */
    VMC_EVENT_LINK_DEGRADED,    /* latency/packet-loss threshold crossed */
    VMC_EVENT_LINK_LOST,        /* connectivity lost */
    VMC_EVENT_USER_TERMINATE,   /* explicit logout / power off */
    VMC_EVENT_SECURITY_TERM,    /* session termination from security events */
    VMC_EVENT_TIMEOUT,          /* discovery/connect attempt expired */
    VMC_EVENT_COUNT
} vmc_session_event;

/* Callbacks fired on state transitions. Optional; may be NULL. */
typedef struct vmc_session_callbacks {
    void (*on_state_change)(vmc_session_state from, vmc_session_state to,
                            void *user);
    void (*on_quality_drop)(void *user);     /* entered DEGRADED */
    void (*on_session_lost)(void *user);     /* entered RECONNECTING */
    void (*on_terminated)(vmc_session_state reason, void *user);
} vmc_session_callbacks;

typedef struct vmc_session {
    vmc_session_state       state;
    vmc_session_callbacks   cb;
    void                   *user;
    u32                     retries;
} vmc_session;

vmc_status vmc_session_init(vmc_session *s, vmc_session_callbacks cb, void *user);

vmc_session_state vmc_session_get_state(const vmc_session *s);

/* Feed an event; returns the resulting state or VMC_ERR_INVALID_ARG. */
vmc_status vmc_session_dispatch(vmc_session *s, vmc_session_event ev);

const char *vmc_session_state_name(vmc_session_state st);
const char *vmc_session_event_name(vmc_session_event ev);

VMC_END_DECLS

#endif /* VMC_SESSION_STATE_H */
