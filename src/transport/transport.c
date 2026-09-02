#include "vmc/transport/transport.h"

void vmc_transport_note_tx(vmc_transport *t, sz_t bytes) {
    t->tx_bytes += (u64)bytes;
    t->tx_packets++;
}

void vmc_transport_note_rx(vmc_transport *t, sz_t bytes) {
    t->rx_bytes += (u64)bytes;
    t->rx_packets++;
}
