/* vim: ts=4 sw=4 et */
#include <rte_ip.h>

#include "natasha.h"


/*
 * @return
 *  - True if ip is configured on port in vlan.
 */
int
is_natasha_port_ip(struct app_config *app_config, uint32_t ip, int vlan,
                   uint8_t port)
{
    struct port_ip_addr *ip_addr;

    ip_addr = app_config->ports[port].ip_addresses;
    while (ip_addr) {
        if (ip_addr->addr.ip == ip && ip_addr->addr.vlan == vlan) {
            return 1;
        }
        ip_addr = ip_addr->next;
    }
    return 0;
}

/*
 * @return
 *  - True if ip is configured on any port in vlan.
 */
int
is_natasha_ip(struct app_config *app_config, uint32_t ip, int vlan)
{
    uint8_t port;

    for (port = 0; port < rte_eth_dev_count(); ++port) {
        if (is_natasha_port_ip(app_config, ip, vlan, port)) {
            return 1;
        }
    }
    return 0;
}

/*
 * Send a burst of output packets on the transmit @queue of @port.
 *
 * Packets that can't be stored in the transmit ring are freed.
 *
 * XXX: we should keep the packets not sent for a later call instead of
 *      discarding them.
 *
 * @return
 *  - The return value of rte_eth_tx_burst(), ie. the number of packets
 *    actually stored in transmit descriptors of the transmit ring.
 */
uint16_t
tx_flush(uint8_t port, struct tx_queue *queue, struct natasha_app_stats *stats)
{
    uint16_t sent;
    uint16_t n;

    if (!queue->len) {
        return 0;
    }

    // rte_eth_tx_prepare updates queue->pkts to offload TCP/UDP checksums.
    //
    // Make sure to set the ol_flag PKT_TX_IPV4 otherwise rte_eth_prepare
    // considers the packet as an IPV6 packet (see
    // dpdk/lib/librte_net/rte_net.h:rte_net_intel_cksum_flags_prepare()).
    //
    // We don't check the return value of rte_eth_tx_prepare because the
    // function rte_net_intel_cksum_flags_prepare() in rte_net.h cannot fail.
    (void)rte_eth_tx_prepare(port, queue->id, queue->pkts, queue->len);

    sent = rte_eth_tx_burst(port, queue->id, queue->pkts, queue->len);

    // rte_eth_tx_burst() is responsible to free the sent packets. We need to
    // free the packets not sent.
    n = sent;
    while (n < queue->len) {
        stats->drop_tx_notsent++;
        rte_pktmbuf_free(queue->pkts[n]);
        n++;
    }

    queue->len = 0;

    return sent;
}

/*
 * Enqueue a packet. If the queue is full, flush it.
 *
 * @return
 *  - The number of packets sent. Zero if the packet is only enqueued and the
 *    queue isn't full, otherwise the value returned by tx_flush().
 */
uint16_t
tx_send(struct rte_mbuf *pkt, uint8_t port, struct tx_queue *queue,
        struct natasha_app_stats *stats)
{
    // Offload VLAN tagging if pkt has a non-zero vlan
    if (pkt->vlan_tci) {
        pkt->ol_flags |= PKT_TX_VLAN_PKT;
    }

    pkt->l2_len = sizeof(struct ether_hdr);
    pkt->l3_len = sizeof(struct ipv4_hdr);

    queue->pkts[queue->len] = pkt;
    queue->len++;

    if (queue->len >= sizeof(queue->pkts) / sizeof(*queue->pkts)) {
        return tx_flush(port, queue, stats);
    }
    return 0;
}
