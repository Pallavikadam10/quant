// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2018, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <arpa/inet.h>
#include <bitstring.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <ev.h>
#include <quant/quant.h>
#include <warpcore/warpcore.h>

#ifdef HAVE_ASAN
#include <sanitizer/asan_interface.h>
#endif

#include "conn.h"
#include "diet.h"
#include "frame.h"
#include "marshall.h"
#include "pkt.h"
#include "quic.h"
#include "recovery.h"
#include "stream.h"


struct ipnp_splay conns_by_ipnp = splay_initializer(&conns_by_ipnp);
struct cid_splay conns_by_cid = splay_initializer(&conns_by_cid);


int ipnp_splay_cmp(const struct q_conn * const a, const struct q_conn * const b)
{
    ensure((a->peer.sin_family == AF_INET || a->peer.sin_family == 0) &&
               (b->peer.sin_family == AF_INET || b->peer.sin_family == 0),
           "limited to AF_INET");

    int diff = memcmp(&a->peer.sin_addr.s_addr, &b->peer.sin_addr.s_addr,
                      sizeof(a->peer.sin_addr.s_addr));
    if (likely(diff))
        return diff;

    diff = (a->peer.sin_port > b->peer.sin_port) -
           (a->peer.sin_port < b->peer.sin_port);
    if (likely(diff))
        return diff;

    // include only the client flag in the comparison
    return a->is_clnt - b->is_clnt;
}


int cid_splay_cmp(const struct q_conn * const a, const struct q_conn * const b)
{
    const int diff = (a->id > b->id) - (a->id < b->id);
    if (likely(diff))
        return diff;
    // include only the client flags in the comparison
    return a->is_clnt - b->is_clnt;
}


SPLAY_GENERATE(ipnp_splay, q_conn, node_ipnp, ipnp_splay_cmp)
SPLAY_GENERATE(cid_splay, q_conn, node_cid, cid_splay_cmp)


static bool __attribute__((const)) vers_supported(const uint32_t v)
{
    for (uint8_t i = 0; i < ok_vers_len; i++)
        if (v == ok_vers[i])
            return true;

    // we're out of matching candidates
    warn(INF, "no vers in common with clnt");
    return false;
}


static uint32_t __attribute__((nonnull))
pick_from_server_vers(const void * const buf, const uint16_t len)
{
    const uint16_t pos = pkt_hdr_len(buf, len);
    for (uint8_t i = 0; i < ok_vers_len; i++)
        for (uint8_t j = 0; j < len - pos; j += sizeof(uint32_t)) {
            uint32_t vers = 0;
            uint16_t x = j + pos;
            dec(&vers, buf, len, x, sizeof(vers), "0x%08x");
            warn(DBG, "serv prio %ld = 0x%08x; our prio %u = 0x%08x",
                 j / sizeof(uint32_t), vers, i, ok_vers[i]);
            if (ok_vers[i] == vers)
                return vers;
        }

    // we're out of matching candidates
    warn(INF, "no vers in common with serv");
    return 0;
}


struct q_conn * get_conn_by_ipnp(const struct sockaddr_in * const peer,
                                 const bool is_clnt)
{
    struct q_conn which = {.peer = *peer, .is_clnt = is_clnt};
    return splay_find(ipnp_splay, &conns_by_ipnp, &which);
}


struct q_conn * get_conn_by_cid(const uint64_t id, const bool is_clnt)
{
    struct q_conn which = {.id = id, .is_clnt = is_clnt};
    return splay_find(cid_splay, &conns_by_cid, &which);
}


static void log_sent_pkts(struct q_conn * const c)
{
    char sent_pkts_buf[1024] = "";
    uint64_t prev = 0;
    for (struct pkt_meta * p = splay_min(pm_nr_splay, &c->rec.sent_pkts); p;
         p = splay_next(pm_nr_splay, &c->rec.sent_pkts, p)) {
        char tmp[1024] = "";
        const bool ack_only = is_ack_only(p);
        snprintf(tmp, sizeof(tmp), "%s%s" FMT_PNR_OUT "%s ",
                 is_rtxable(p) ? "*" : "", ack_only ? "(" : "",
                 shorten_ack_nr(p->nr, p->nr - prev), ack_only ? ")" : "");
        strncat(sent_pkts_buf, tmp,
                sizeof(sent_pkts_buf) - strlen(sent_pkts_buf) - 1);
        prev = p->nr;
    }
    warn(DBG, "unacked: %s", sent_pkts_buf);
}


static uint32_t __attribute__((nonnull(1))) tx_stream(struct q_stream * const s,
                                                      const bool rtx,
                                                      const uint32_t limit,
                                                      struct w_iov * const from)
{
    struct w_iov_sq x = sq_head_initializer(x);
    uint32_t encoded = 0;
    struct w_iov * v = from;
    sq_foreach_from (v, &s->out, next) {
        if (v->len > Q_OFFSET && (s->blocked || s->c->blocked))
            break;

        if (meta(v).is_acked) {
            warn(DBG,
                 "skipping ACKed pkt " FMT_PNR_OUT " (%" PRIx64
                 ") on str " FMT_SID " during %s",
                 meta(v).nr, meta(v).nr, s->id, rtx ? "RTX" : "TX");
            continue;
        }

        if (rtx != (meta(v).tx_len > 0)) {
            warn(DBG,
                 "skipping %s pkt " FMT_PNR_OUT " (%" PRIx64 ") on str " FMT_SID
                 " during %s",
                 meta(v).tx_len ? "already-tx'ed" : "fresh", meta(v).nr,
                 meta(v).nr, s->id, rtx ? "RTX" : "TX");
            continue;
        }

        if (rtx) {
            ensure(meta(v).is_rtxed == false, "cannot RTX an RTX");
            // on RTX, remember orig pkt meta data
            struct w_iov * const r =
                q_alloc_iov(w_engine(s->c->sock), 0, Q_OFFSET);
            pm_cpy(&meta(r), &meta(v));                  // copy pkt meta data
            memcpy(r->buf, v->buf - Q_OFFSET, Q_OFFSET); // copy pkt headers
            meta(r).is_rtxed = true;

            // we reinsert meta(v) with its new pkt nr in on_pkt_sent()
            splay_remove(pm_nr_splay, &s->c->rec.sent_pkts, &meta(v));
            splay_insert(pm_nr_splay, &s->c->rec.sent_pkts, &meta(r));
        }

        if (s->c->state >= CONN_STAT_ESTB) {
            // if we have less than two full packet's worth of window, block
            if (s->out_data + 2 * MAX_PKT_LEN > s->out_data_max)
                s->blocked = true;
            if (s->c->out_data + 2 * MAX_PKT_LEN > s->c->peer_max_data)
                s->c->blocked = true;
        }

        enc_pkt(s, rtx, v, &x);
        on_pkt_sent(s->c, v);
        encoded++;

        if (limit && encoded == limit) {
            warn(NTE, "tx limit %u reached", limit);
            break;
        }
    }

    if (encoded) {
        // transmit encrypted/protected packets and then free the chain
        if (!s->c->is_clnt)
            w_connect(s->c->sock, s->c->peer.sin_addr.s_addr,
                      s->c->peer.sin_port);
        w_tx(s->c->sock, &x);
        w_nic_tx(w_engine(s->c->sock));
        if (!s->c->is_clnt)
            w_disconnect(s->c->sock);
        q_free(&x);
    }

    log_sent_pkts(s->c);
    return encoded;
}


static uint32_t
tx_other(struct q_stream * const s, const bool rtx, const uint32_t limit)
{
    warn(DBG,
         "other %s on %s conn " FMT_CID " str " FMT_SID " w/%u pkt%s in queue",
         rtx ? "RTX" : "TX", conn_type(s->c), s->c->id, s->id, sq_len(&s->out),
         plural(sq_len(&s->out)));

    struct w_iov *v = 0, *last = 0;
    if (!rtx) {
        v = q_alloc_iov(w_engine(s->c->sock), 0, Q_OFFSET);
        v->len = 0; // this packet will have no stream data
        last = sq_last(&s->out, w_iov, next);
        sq_insert_tail(&s->out, v, next);
    }

    const bool did_tx = tx_stream(s, rtx, limit, v);

    if (!rtx && !is_rtxable(&meta(v))) {
        ensure(sq_last(&s->out, w_iov, next) == v, "queue mixed up");
        if (last)
            sq_remove_after(&s->out, last, next);
        else
            sq_remove_head(&s->out, next);
        // if (s->c->state == CONN_STAT_VERS_REJ)
        //     // we can free the version negotiation response
        //     q_free_iov(v);
    }

    return did_tx;
}


void tx(struct q_conn * const c, const bool rtx, const uint32_t limit)
{
    bool did_tx = false;
    struct q_stream * s = 0;
    splay_foreach (s, stream, &c->streams) {
        // check if we need to open stream or connection windows
        s->open_win |= s->in_data == s->in_data_max;
        s->c->open_win |= s->c->in_data == s->c->local_max_data;

        if (!s->blocked && !s->c->blocked && !sq_empty(&s->out) &&
            sq_len(&s->out) > s->out_ack_cnt) {
            warn(DBG,
                 "data %s on %s conn " FMT_CID " str " FMT_SID
                 " w/%u pkt%s in queue",
                 rtx ? "RTX" : "TX", conn_type(c), c->id, s->id,
                 sq_len(&s->out), plural(sq_len(&s->out)));
            did_tx |= tx_stream(s, rtx, limit, 0);
        }
        if (did_tx == false &&
            (s->open_win == true || s->c->open_win == true ||
             ((s->state == STRM_STAT_HCLO || s->state == STRM_STAT_CLSD) &&
              s->fin_sent == false)))
            did_tx |= tx_other(s, rtx, limit);
    }

    if (did_tx == false) {
        // need to ACK w/o any stream data to piggyback on; so abuse stream 0
        s = get_stream(c, 0);
        ensure(s, "no stream 0");
        tx_other(s, rtx, limit);
    }
}


void tx_w(struct ev_loop * const l __attribute__((unused)),
          ev_async * const w,
          int e __attribute__((unused)))
{
    tx(w->data, false, 0);
}


static void __attribute__((nonnull))
update_cid(struct q_conn * const c, const uint64_t cid)
{
    splay_remove(cid_splay, &conns_by_cid, c);
    c->id = cid;
    splay_insert(cid_splay, &conns_by_cid, c);
}


static void __attribute__((nonnull))
update_ipnp(struct q_conn * const c, const struct sockaddr_in * const peer)
{
    splay_remove(ipnp_splay, &conns_by_ipnp, c);
    c->peer = *peer;
    splay_insert(ipnp_splay, &conns_by_ipnp, c);
}


static bool __attribute__((nonnull))
verify_prot(struct q_conn * const c, struct w_iov * const v)
{
    const uint8_t flags = pkt_flags(v->buf);
    if (is_set(F_LONG_HDR, flags) && pkt_vers(v->buf, v->len) == 0)
        // version negotiation responses do not carry protection
        return true;

    const uint16_t hdr_len = pkt_hdr_len(v->buf, v->len);
    const uint16_t len = dec_aead(c, v, hdr_len);
    if (len == 0)
        return false;
    v->len -= v->len - len;
    return true;
}


static void __attribute__((nonnull))
track_recv(struct q_conn * const c, const uint64_t nr)
{
    diet_insert(&c->recv, nr);
    if (nr == diet_max(&c->recv))
        c->lg_recv_t = ev_now(loop);
}


static void __attribute__((nonnull))
process_pkt(struct q_conn * const c, struct w_iov * const v)
{
    const uint8_t flags = pkt_flags(v->buf);

    if (c->is_clnt && c->state == CONN_STAT_IDLE)
        conn_to_state(c, CONN_STAT_CH_SENT);

    switch (c->state) {
    case CONN_STAT_IDLE:
    case CONN_STAT_VERS_REJ:
        // validate minimum packet size
        if (v->len < MIN_INI_LEN)
            warn(ERR, "initial %u-byte pkt too short (< %u)", v->len,
                 MIN_INI_LEN);

        ensure(is_set(F_LONG_HDR, flags), "have a long header");

        // respond to the version negotiation packet
        c->vers = pkt_vers(v->buf, v->len);
        c->needs_tx = true;
        track_recv(c, meta(v).nr);
        if (c->vers_initial == 0)
            c->vers_initial = c->vers;
        if (vers_supported(c->vers) && !is_force_neg_vers(c->vers)) {
            warn(INF, "supporting clnt-requested vers 0x%08x", c->vers);

            init_cleartext_prot(c);
            if (verify_prot(c, v) == false)
                goto done;
#ifndef NO_RANDOM_CID
            // this is a new connection; server picks a new random cid
            uint64_t cid;
            arc4random_buf(&cid, sizeof(cid));
            warn(NTE, "picked new cid " FMT_CID " for %s conn " FMT_CID, cid,
                 conn_type(c), c->id);
            update_cid(c, cid);
#endif
            init_tls(c);
            dec_frames(c, v);

        } else {
            conn_to_state(c, CONN_STAT_VERS_REJ);
            warn(WRN,
                 "%s conn " FMT_CID
                 " clnt-requested vers 0x%08x not supported ",
                 conn_type(c), c->id, c->vers);
        }
        break;

    case CONN_STAT_CH_SENT:
        if (pkt_vers(v->buf, v->len) == 0) {
            const uint32_t try_vers = pick_from_server_vers(v->buf, v->len);
            if (try_vers == c->vers) {
                warn(INF, "ignoring spurious vers neg response");
                break;
            }

            warn(INF, "serv didn't like our vers 0x%08x", c->vers);
            // TODO: check CID

            if (c->vers_initial == 0)
                c->vers_initial = c->vers;
            c->vers = try_vers;
            if (c->vers)
                warn(INF, "retrying with vers 0x%08x", c->vers);
            else
                die("no vers in common with serv");

            // retransmit the ClientHello
            conn_to_state(c, CONN_STAT_IDLE);
            init_tls(c);
            // free the previous ClientHello
            struct pkt_meta *ch, *nxt;
            for (ch = splay_min(pm_nr_splay, &c->rec.sent_pkts); ch; ch = nxt) {
                nxt = splay_next(pm_nr_splay, &c->rec.sent_pkts, ch);
                c->rec.in_flight -= ch->tx_len;
                splay_remove(pm_nr_splay, &c->rec.sent_pkts, ch);
            }
            struct q_stream * s = get_stream(c, 0);
            q_free(&s->out);
            s->out_off = 0;
            tls_io(s, 0);
            c->needs_tx = true;

        } else {
            warn(INF, "serv accepted vers 0x%08x", c->vers);
            if (verify_prot(c, v) == false)
                return;

            dec_frames(c, v);

            if (pkt_type(flags) == F_LH_RTRY) {
                warn(INF, "handling serv stateless retry");
                conn_to_state(c, CONN_STAT_RTRY);
                // reset stream 0 offsets
                struct q_stream * s = get_stream(c, 0);
                s->out_off = s->in_off = 0;
            } else
                track_recv(c, meta(v).nr);
        }
        break;

    case CONN_STAT_RTRY:
        if (verify_prot(c, v) == false)
            goto done;
        track_recv(c, meta(v).nr);
        dec_frames(c, v);
        break;

    case CONN_STAT_HSHK_DONE:
        if (is_set(F_LONG_HDR, flags) && pkt_vers(v->buf, v->len) == 0) {
            // we shouldn't get another version negotiation packet here, ignore
            warn(NTE, "ignoring spurious ver neg response");
            goto done;
        }

        // pass any further data received on stream 0 to TLS and check
        // whether that completes the client handshake
        if (!is_set(F_LONG_HDR, flags) || pkt_type(flags) <= F_LH_HSHK)
            maybe_api_return(q_accept, c);
        // fall through

    case CONN_STAT_ESTB:
    case CONN_STAT_CLNG:
    case CONN_STAT_HSHK_FAIL:
    case CONN_STAT_DRNG:
        if (verify_prot(c, v) == false) {
            // check if this is a stateless reset
            if (memcmp(&v->buf[v->len - 16], c->stateless_reset_token, 16) ==
                0) {
                warn(NTE, "stateless reset on %s conn " FMT_CID, conn_type(c),
                     c->id);
                conn_to_state(c, CONN_STAT_DRNG);
            }
            goto done;
        }
        track_recv(c, meta(v).nr);
        dec_frames(c, v);
        break;

    default:
        die("TODO: state %u", c->state);
    }

done:
    if (is_rtxable(&meta(v)) == false)
        q_free_iov(v);
}


void rx(struct ev_loop * const l,
        ev_io * const rx_w,
        int e __attribute__((unused)))
{
    // read from NIC
    struct w_sock * const ws = rx_w->data;
    struct w_engine * const w = w_engine(ws);
    w_nic_rx(w, -1);
    struct w_iov_sq i = sq_head_initializer(i);
    sl_head(, q_conn) crx = sl_head_initializer();
    w_rx(ws, &i);

    while (!sq_empty(&i)) {
        struct w_iov * const v = sq_first(&i);
        ASAN_UNPOISON_MEMORY_REGION(&meta(v), sizeof(meta(v)));
        sq_remove_head(&i, next);

        if (v->len > MAX_PKT_LEN)
            warn(WRN, "received %u-byte pkt (> %u max)", v->len, MAX_PKT_LEN);
        const uint16_t hdr_len = pkt_hdr_len(v->buf, v->len);
        if (v->len < hdr_len) {
            warn(ERR, "%u-byte pkt indicates %u-byte hdr; ignoring", v->len,
                 hdr_len);
            q_free_iov(v);
            continue;
        }

        const uint8_t flags = pkt_flags(v->buf);
        const bool is_clnt = w_connected(ws);
        uint64_t cid = 0;
        struct q_conn * c = 0;

        if (is_set(F_LONG_HDR, flags) || !is_set(F_SH_OMIT_CID, flags)) {
            cid = pkt_cid(v->buf, v->len);
            c = get_conn_by_cid(cid, is_clnt);
        }

        if (c == 0) {
            const struct sockaddr_in peer = {.sin_family = AF_INET,
                                             .sin_port = v->port,
                                             .sin_addr = {.s_addr = v->ip}};
            if (is_set(F_LONG_HDR, flags)) {
                if (is_clnt) {
                    // server may have picked a new cid
                    c = get_conn_by_ipnp(&peer, is_clnt);
                    warn(DBG, "got new cid " FMT_CID " for %s conn " FMT_CID,
                         cid, conn_type(c), c->id);
                    update_cid(c, cid);
                } else {
                    warn(NTE, "new serv conn from %s:%u",
                         inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
                    const struct sockaddr_in none = {0};
                    c = get_conn_by_ipnp(&none, is_clnt);
                    if (c == 0) {
                        // TODO: maintain accept queue
                        warn(CRT, "app is not in q_accept(), ignoring");
                        q_free_iov(v);
                        continue;
                    }
                    update_ipnp(c, &peer);
                    update_cid(c, cid);
                    new_stream(c, 0, false);
                }

            } else
                c = get_conn_by_ipnp(&peer, is_clnt);
        }

        if (c == 0) {
            warn(INF, "cannot find connection for 0x%02x packet", flags);
            q_free_iov(v);
            continue;
        }

        meta(v).nr = pkt_nr(v->buf, v->len, c);
        log_pkt("RX", v);

        // remember that we had a RX event on this connection
        if (!c->had_rx) {
            c->had_rx = true;
            sl_insert_head(&crx, c, next);
        }

        process_pkt(c, v);
    }

    // for all connections that had RX events
    while (!sl_empty(&crx)) {
        struct q_conn * const c = sl_first(&crx);
        sl_remove_head(&crx, next);

        // process stream 0
        struct q_stream * const s = get_stream(c, 0);
        while (!sq_empty(&s->in)) {
            struct w_iov * iv = sq_first(&s->in);
            sq_remove_head(&s->in, next);
            if (tls_io(s, iv) == 0)
                maybe_api_return(q_connect, c);
            q_free_iov(iv);
        }

        // reset idle timeout
        ev_timer_again(l, &c->idle_alarm);

        // is a TX needed for this connection?
        if (c->needs_tx)
            tx(c, false, 0);
        else
            log_sent_pkts(c);

        // clear the helper flags set above
        c->needs_tx = c->had_rx = false;
    }
}


void err_close(struct q_conn * const c,
               const uint16_t code,
               const char * const fmt,
               ...)
{
    va_list ap;
    va_start(ap, fmt);

    char reas[256];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    const int ret = vsnprintf(reas, sizeof(reas), fmt, ap);
#pragma clang diagnostic pop
    ensure(ret >= 0, "vsnprintf() failed");
    va_end(ap);

    if (c->err_code) {
        warn(WRN, "ignoring new err 0x%04x (%s); existing err is 0x%04x (%s) ",
             code, reas, c->err_code, c->err_reason);
        return;
    }

    warn(ERR, "%s", reas);
    c->err_code = code;
    c->err_reason = strdup(reas);
    conn_to_state(c, c->state <= CONN_STAT_HSHK_DONE ? CONN_STAT_HSHK_FAIL
                                                     : CONN_STAT_CLNG);
}


static void __attribute__((nonnull))
enter_closed(struct ev_loop * const l __attribute__((unused)),
             ev_timer * const w,
             int e __attribute__((unused)))
{
    struct q_conn * const c = w->data;
    maybe_api_return(q_close, c);
}


void conn_to_state(struct q_conn * const c, const uint8_t state)
{
    warn(DBG, "conn %" PRIx64 " state %u -> %u", c->id, c->state, state);
    c->state = state;

    switch (state) {
    case CONN_STAT_IDLE:
    case CONN_STAT_CH_SENT:
    case CONN_STAT_VERS_REJ:
    case CONN_STAT_RTRY:
    case CONN_STAT_HSHK_DONE:
    case CONN_STAT_HSHK_FAIL:
        break;
    case CONN_STAT_ESTB:
        c->needs_tx = true;
        break;
    case CONN_STAT_DRNG:
    case CONN_STAT_CLNG:
        // stop LD alarm
        ev_timer_stop(loop, &c->rec.ld_alarm);

        c->closing_alarm.data = c;
        c->closing_alarm.repeat = 3; // TODO: 3 * RTO
        ev_init(&c->closing_alarm, enter_closed);
        ev_timer_again(loop, &c->closing_alarm);
        break;
    default:
        die("unhandled state %u", state);
    }
}
