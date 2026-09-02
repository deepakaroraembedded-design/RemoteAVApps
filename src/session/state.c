#include "vmc/session/state.h"

#include "vmc/core/error.h"

static const char *const k_state_names[VMC_SESSION_COUNT] = {
    "OFFLINE", "DISCOVER", "CONNECTING", "ACTIVE",
    "DEGRADED", "RECONNECTING", "TERMINATING",
};

static const char *const k_event_names[VMC_EVENT_COUNT] = {
    "BOOT", "MAPPER_RESP", "TRANSPORT_UP", "LINK_OK",
    "LINK_DEGRADED", "LINK_LOST", "USER_TERMINATE",
    "SECURITY_TERM", "TIMEOUT",
};

const char *vmc_session_state_name(vmc_session_state st) {
    return (st < VMC_SESSION_COUNT) ? k_state_names[st] : "?";
}

const char *vmc_session_event_name(vmc_session_event ev) {
    return (ev < VMC_EVENT_COUNT) ? k_event_names[ev] : "?";
}

vmc_status vmc_session_init(vmc_session *s, vmc_session_callbacks cb, void *user) {
    if (!s) return VMC_ERR_INVALID_ARG;
    s->state   = VMC_SESSION_OFFLINE;
    s->cb      = cb;
    s->user    = user;
    s->retries = 0;
    return VMC_OK;
}

vmc_session_state vmc_session_get_state(const vmc_session *s) {
    return s ? s->state : VMC_SESSION_OFFLINE;
}

static void transition(vmc_session *s, vmc_session_state to) {
    vmc_session_state from = s->state;
    if (from == to) return;
    s->state = to;
    if (s->cb.on_state_change) {
        s->cb.on_state_change(from, to, s->user);
    }
    switch (to) {
        case VMC_SESSION_DEGRADED:
            if (s->cb.on_quality_drop) s->cb.on_quality_drop(s->user);
            break;
        case VMC_SESSION_RECONNECTING:
            if (s->cb.on_session_lost) s->cb.on_session_lost(s->user);
            break;
        default:
            break;
    }
}

vmc_status vmc_session_dispatch(vmc_session *s, vmc_session_event ev) {
    if (!s) return VMC_ERR_INVALID_ARG;

    switch (s->state) {
        case VMC_SESSION_OFFLINE:
            if (ev == VMC_EVENT_BOOT) {
                transition(s, VMC_SESSION_DISCOVER);
                return VMC_OK;
            }
            break;

        case VMC_SESSION_DISCOVER:
            if (ev == VMC_EVENT_MAPPER_RESP) {
                transition(s, VMC_SESSION_CONNECTING);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_TIMEOUT) {
                s->retries++;
                return VMC_OK; /* remain DISCOVER, caller retries */
            }
            if (ev == VMC_EVENT_LINK_LOST) {
                transition(s, VMC_SESSION_OFFLINE);
                return VMC_OK;
            }
            break;

        case VMC_SESSION_CONNECTING:
            if (ev == VMC_EVENT_TRANSPORT_UP) {
                transition(s, VMC_SESSION_ACTIVE);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_TIMEOUT) {
                s->retries++;
                transition(s, VMC_SESSION_DISCOVER);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_LINK_LOST) {
                transition(s, VMC_SESSION_OFFLINE);
                return VMC_OK;
            }
            break;

        case VMC_SESSION_ACTIVE:
            if (ev == VMC_EVENT_LINK_DEGRADED) {
                transition(s, VMC_SESSION_DEGRADED);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_LINK_LOST) {
                transition(s, VMC_SESSION_RECONNECTING);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_USER_TERMINATE || ev == VMC_EVENT_SECURITY_TERM) {
                transition(s, VMC_SESSION_TERMINATING);
                if (s->cb.on_terminated) s->cb.on_terminated(s->state, s->user);
                return VMC_OK;
            }
            break;

        case VMC_SESSION_DEGRADED:
            if (ev == VMC_EVENT_LINK_OK) {
                transition(s, VMC_SESSION_ACTIVE);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_LINK_LOST) {
                transition(s, VMC_SESSION_RECONNECTING);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_USER_TERMINATE || ev == VMC_EVENT_SECURITY_TERM) {
                transition(s, VMC_SESSION_TERMINATING);
                if (s->cb.on_terminated) s->cb.on_terminated(s->state, s->user);
                return VMC_OK;
            }
            break;

        case VMC_SESSION_RECONNECTING:
            if (ev == VMC_EVENT_TRANSPORT_UP) {
                transition(s, VMC_SESSION_ACTIVE);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_LINK_OK) {
                transition(s, VMC_SESSION_DISCOVER);
                return VMC_OK;
            }
            if (ev == VMC_EVENT_TIMEOUT) {
                s->retries++;
                transition(s, VMC_SESSION_OFFLINE);
                return VMC_OK;
            }
            break;

        case VMC_SESSION_TERMINATING:
            /* Terminal state; no transitions out. */
            return VMC_OK;

        default:
            break;
    }
    return VMC_ERR_INVALID_STATE;
}
