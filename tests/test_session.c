#include <string.h>

#include "vmc_test.h"
#include "vmc/session/state.h"
#include "vmc/core/error.h"

static int g_state_changes = 0;
static vmc_session_state g_last_from;
static vmc_session_state g_last_to;
static int g_term_calls = 0;

static void on_state(vmc_session_state from, vmc_session_state to, void *u) {
    (void)u;
    g_state_changes++;
    g_last_from = from;
    g_last_to = to;
}

static void on_term(vmc_session_state reason, void *u) {
    (void)u;
    g_term_calls++;
    CHECK_EQ(reason, VMC_SESSION_TERMINATING);
}

static void reset_counters(void) {
    g_state_changes = 0;
    g_term_calls = 0;
}

static void test_nominal_lifecycle(void) {
    vmc_session s;
    vmc_session_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_state_change = on_state;
    cb.on_terminated = on_term;
    reset_counters();

    CHECK_EQ(vmc_session_init(&s, cb, NULL), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_OFFLINE);

    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_BOOT), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_DISCOVER);

    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_MAPPER_RESP), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_CONNECTING);

    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_TRANSPORT_UP), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_ACTIVE);

    /* Degrade and recover. */
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_LINK_DEGRADED), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_DEGRADED);
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_LINK_OK), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_ACTIVE);

    /* Terminate. */
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_USER_TERMINATE), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_TERMINATING);
    CHECK_EQ(g_term_calls, 1);
}

static void test_link_lost_paths(void) {
    vmc_session s;
    vmc_session_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    CHECK_EQ(vmc_session_init(&s, cb, NULL), VMC_OK);

    /* From ACTIVE -> RECONNECTING -> (reconnect) ACTIVE */
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_BOOT), VMC_OK);
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_MAPPER_RESP), VMC_OK);
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_TRANSPORT_UP), VMC_OK);

    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_LINK_LOST), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_RECONNECTING);

    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_TRANSPORT_UP), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_ACTIVE);

    /* Give up on reconnect -> OFFLINE. */
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_LINK_LOST), VMC_OK);
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_TIMEOUT), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_OFFLINE);
    CHECK_EQ(s.retries, 1u);
}

static void test_illegal_transition_rejected(void) {
    vmc_session s;
    vmc_session_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    CHECK_EQ(vmc_session_init(&s, cb, NULL), VMC_OK);

    /* TRANSPORT_UP from OFFLINE is not a valid transition. */
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_TRANSPORT_UP), VMC_ERR_INVALID_STATE);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_OFFLINE);

    /* Timeout from OFFLINE likewise invalid. */
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_TIMEOUT), VMC_ERR_INVALID_STATE);
}

static void test_terminating_is_terminal(void) {
    vmc_session s;
    vmc_session_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    CHECK_EQ(vmc_session_init(&s, cb, NULL), VMC_OK);
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_BOOT), VMC_OK);
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_MAPPER_RESP), VMC_OK);
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_TRANSPORT_UP), VMC_OK);
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_SECURITY_TERM), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_TERMINATING);

    /* No event escapes the terminal state. */
    CHECK_EQ(vmc_session_dispatch(&s, VMC_EVENT_BOOT), VMC_OK);
    CHECK_EQ(vmc_session_get_state(&s), VMC_SESSION_TERMINATING);
}

int main(void) {
    TEST_RUN(test_nominal_lifecycle);
    TEST_RUN(test_link_lost_paths);
    TEST_RUN(test_illegal_transition_rejected);
    TEST_RUN(test_terminating_is_terminal);
    TEST_SUMMARY();
}
