/*
 * Copyright (c) 2008, 2009 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "netflow.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "collectors.h"
#include "flow.h"
#include "netflow.h"
#include "ofpbuf.h"
#include "ofproto.h"
#include "packets.h"
#include "socket-util.h"
#include "svec.h"
#include "timeval.h"
#include "util.h"
#include "xtoxll.h"

#define THIS_MODULE VLM_netflow
#include "vlog.h"

#define NETFLOW_V5_VERSION 5

static const int ACTIVE_TIMEOUT_DEFAULT = 600;

/* Every NetFlow v5 message contains the header that follows.  This is
 * followed by up to thirty records that describe a terminating flow.
 * We only send a single record per NetFlow message.
 */
struct netflow_v5_header {
    uint16_t version;              /* NetFlow version is 5. */
    uint16_t count;                /* Number of records in this message. */
    uint32_t sysuptime;            /* System uptime in milliseconds. */
    uint32_t unix_secs;            /* Number of seconds since Unix epoch. */
    uint32_t unix_nsecs;           /* Number of residual nanoseconds
                                      after epoch seconds. */
    uint32_t flow_seq;             /* Number of flows since sending
                                      messages began. */
    uint8_t  engine_type;          /* Engine type. */
    uint8_t  engine_id;            /* Engine id. */
    uint16_t sampling_interval;    /* Set to zero. */
};
BUILD_ASSERT_DECL(sizeof(struct netflow_v5_header) == 24);

/* A NetFlow v5 description of a terminating flow.  It is preceded by a
 * NetFlow v5 header.
 */
struct netflow_v5_record {
    uint32_t src_addr;             /* Source IP address. */
    uint32_t dst_addr;             /* Destination IP address. */
    uint32_t nexthop;              /* IP address of next hop.  Set to 0. */
    uint16_t input;                /* Input interface index. */
    uint16_t output;               /* Output interface index. */
    uint32_t packet_count;         /* Number of packets. */
    uint32_t byte_count;           /* Number of bytes. */
    uint32_t init_time;            /* Value of sysuptime on first packet. */
    uint32_t used_time;            /* Value of sysuptime on last packet. */

    /* The 'src_port' and 'dst_port' identify the source and destination
     * port, respectively, for TCP and UDP.  For ICMP, the high-order
     * byte identifies the type and low-order byte identifies the code
     * in the 'dst_port' field. */
    uint16_t src_port;
    uint16_t dst_port;

    uint8_t  pad1;
    uint8_t  tcp_flags;            /* Union of seen TCP flags. */
    uint8_t  ip_proto;             /* IP protocol. */
    uint8_t  ip_tos;               /* IP TOS value. */
    uint16_t src_as;               /* Source AS ID.  Set to 0. */
    uint16_t dst_as;               /* Destination AS ID.  Set to 0. */
    uint8_t  src_mask;             /* Source mask bits.  Set to 0. */
    uint8_t  dst_mask;             /* Destination mask bits.  Set to 0. */
    uint8_t  pad[2];
};
BUILD_ASSERT_DECL(sizeof(struct netflow_v5_record) == 48);

struct netflow {
    uint8_t engine_type;          /* Value of engine_type to use. */
    uint8_t engine_id;            /* Value of engine_id to use. */
    long long int boot_time;      /* Time when netflow_create() was called. */
    struct collectors *collectors; /* NetFlow collectors. */
    bool add_id_to_iface;         /* Put the 7 least signficiant bits of 
                                   * 'engine_id' into the most signficant 
                                   * bits of the interface fields. */
    uint32_t netflow_cnt;         /* Flow sequence number for NetFlow. */
    struct ofpbuf packet;         /* NetFlow packet being accumulated. */
    long long int active_timeout; /* Timeout for flows that are still active. */
    long long int reconfig_time;  /* When we reconfigured the timeouts. */
};

void
netflow_expire(struct netflow *nf, struct netflow_flow *nf_flow,
               struct ofexpired *expired)
{
    struct netflow_v5_header *nf_hdr;
    struct netflow_v5_record *nf_rec;
    struct timeval now;

    nf_flow->last_expired += nf->active_timeout;

    /* NetFlow only reports on IP packets and we should only report flows
     * that actually have traffic. */
    if (expired->flow.dl_type != htons(ETH_TYPE_IP) ||
        expired->packet_count - nf_flow->packet_count_off == 0) {
        return;
    }

    time_timeval(&now);

    if (!nf->packet.size) {
        nf_hdr = ofpbuf_put_zeros(&nf->packet, sizeof *nf_hdr);
        nf_hdr->version = htons(NETFLOW_V5_VERSION);
        nf_hdr->count = htons(0);
        nf_hdr->sysuptime = htonl(time_msec() - nf->boot_time);
        nf_hdr->unix_secs = htonl(now.tv_sec);
        nf_hdr->unix_nsecs = htonl(now.tv_usec * 1000);
        nf_hdr->flow_seq = htonl(nf->netflow_cnt++);
        nf_hdr->engine_type = nf->engine_type;
        nf_hdr->engine_id = nf->engine_id;
        nf_hdr->sampling_interval = htons(0);
    }

    nf_hdr = nf->packet.data;
    nf_hdr->count = htons(ntohs(nf_hdr->count) + 1);

    nf_rec = ofpbuf_put_zeros(&nf->packet, sizeof *nf_rec);
    nf_rec->src_addr = expired->flow.nw_src;
    nf_rec->dst_addr = expired->flow.nw_dst;
    nf_rec->nexthop = htons(0);
    if (nf->add_id_to_iface) {
        uint16_t iface = (nf->engine_id & 0x7f) << 9;
        nf_rec->input = htons(iface | (expired->flow.in_port & 0x1ff));
        nf_rec->output = htons(iface | (nf_flow->output_iface & 0x1ff));
    } else {
        nf_rec->input = htons(expired->flow.in_port);
        nf_rec->output = htons(nf_flow->output_iface);
    }
    nf_rec->packet_count = htonl(MIN(expired->packet_count -
                                     nf_flow->packet_count_off, UINT32_MAX));
    nf_rec->byte_count = htonl(MIN(expired->byte_count -
                                   nf_flow->byte_count_off, UINT32_MAX));
    nf_rec->init_time = htonl(nf_flow->created - nf->boot_time);
    nf_rec->used_time = htonl(MAX(nf_flow->created, expired->used)
                             - nf->boot_time);
    if (expired->flow.nw_proto == IP_TYPE_ICMP) {
        /* In NetFlow, the ICMP type and code are concatenated and
         * placed in the 'dst_port' field. */
        uint8_t type = ntohs(expired->flow.tp_src);
        uint8_t code = ntohs(expired->flow.tp_dst);
        nf_rec->src_port = htons(0);
        nf_rec->dst_port = htons((type << 8) | code);
    } else {
        nf_rec->src_port = expired->flow.tp_src;
        nf_rec->dst_port = expired->flow.tp_dst;
    }
    nf_rec->tcp_flags = nf_flow->tcp_flags;
    nf_rec->ip_proto = expired->flow.nw_proto;
    nf_rec->ip_tos = nf_flow->ip_tos;

    /* Update flow tracking data. */
    nf_flow->created = 0;
    nf_flow->packet_count_off = expired->packet_count;
    nf_flow->byte_count_off = expired->byte_count;
    nf_flow->tcp_flags = 0;

    /* NetFlow messages are limited to 30 records. */
    if (ntohs(nf_hdr->count) >= 30) {
        netflow_run(nf);
    }
}

void
netflow_run(struct netflow *nf)
{
    if (nf->packet.size) {
        collectors_send(nf->collectors, nf->packet.data, nf->packet.size);
        nf->packet.size = 0;
    }
}

int
netflow_set_options(struct netflow *nf,
                    const struct netflow_options *nf_options)
{
    int error = 0;
    long long int old_timeout;

    nf->engine_type = nf_options->engine_type;
    nf->engine_id = nf_options->engine_id;
    nf->add_id_to_iface = nf_options->add_id_to_iface;

    collectors_destroy(nf->collectors);
    collectors_create(&nf_options->collectors, 0, &nf->collectors);

    old_timeout = nf->active_timeout;
    if (nf_options->active_timeout > 0) {
        nf->active_timeout = nf_options->active_timeout;
    } else {
        nf->active_timeout = ACTIVE_TIMEOUT_DEFAULT;
    }
    nf->active_timeout *= 1000;
    if (old_timeout != nf->active_timeout) {
        nf->reconfig_time = time_msec();
    }

    return error;
}

struct netflow *
netflow_create(void)
{
    struct netflow *nf = xmalloc(sizeof *nf);
    nf->engine_type = 0;
    nf->engine_id = 0;
    nf->boot_time = time_msec();
    nf->collectors = NULL;
    nf->add_id_to_iface = false;
    nf->netflow_cnt = 0;
    ofpbuf_init(&nf->packet, 1500);
    return nf;
}

void
netflow_destroy(struct netflow *nf)
{
    if (nf) {
        ofpbuf_uninit(&nf->packet);
        collectors_destroy(nf->collectors);
        free(nf);
    }
}

void
netflow_flow_clear(struct netflow_flow *nf_flow)
{
    uint16_t output_iface = nf_flow->output_iface;

    memset(nf_flow, 0, sizeof *nf_flow);
    nf_flow->output_iface = output_iface;
}

void
netflow_flow_update_time(struct netflow *nf, struct netflow_flow *nf_flow,
                         long long int used)
{
    if (!nf_flow->created) {
        nf_flow->created = used;
    }

    if (!nf || !nf->active_timeout || !nf_flow->last_expired ||
        nf->reconfig_time > nf_flow->last_expired) {
        /* Keep the time updated to prevent a flood of expiration in
         * the future. */
        nf_flow->last_expired = time_msec();
    }
}

void
netflow_flow_update_flags(struct netflow_flow *nf_flow, uint8_t ip_tos,
                          uint8_t tcp_flags)
{
    nf_flow->ip_tos = ip_tos;
    nf_flow->tcp_flags |= tcp_flags;
}

bool
netflow_active_timeout_expired(struct netflow *nf, struct netflow_flow *nf_flow)
{
    if (nf->active_timeout) {
        return time_msec() > nf_flow->last_expired + nf->active_timeout;
    }

    return false;
}
