#if defined __APPLE__
#define __APPLE_USE_RFC_3542
#endif

#if defined __linux__ && !defined _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mud.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <sodium.h>

#if !defined MSG_CONFIRM
#define MSG_CONFIRM 0
#endif

#if defined __linux__
#define MUD_V4V6 1
#else
#define MUD_V4V6 0
#endif

#if defined IP_PKTINFO
#define MUD_PKTINFO IP_PKTINFO
#define MUD_PKTINFO_SRC(X) &((struct in_pktinfo *)(X))->ipi_addr
#define MUD_PKTINFO_DST(X) &((struct in_pktinfo *)(X))->ipi_spec_dst
#define MUD_PKTINFO_SIZE sizeof(struct in_pktinfo)
#elif defined IP_RECVDSTADDR
#define MUD_PKTINFO IP_RECVDSTADDR
#define MUD_PKTINFO_SRC(X) (X)
#define MUD_PKTINFO_DST(X) (X)
#define MUD_PKTINFO_SIZE sizeof(struct in_addr)
#endif

#if defined IP_MTU_DISCOVER
#define MUD_DFRAG IP_MTU_DISCOVER
#define MUD_DFRAG_OPT IP_PMTUDISC_PROBE
#elif defined IP_DONTFRAG
#define MUD_DFRAG IP_DONTFRAG
#define MUD_DFRAG_OPT 1
#endif

#define MUD_ONE_MSEC (UINT64_C(1000))
#define MUD_ONE_SEC  (1000 * MUD_ONE_MSEC)
#define MUD_ONE_MIN  (60 * MUD_ONE_SEC)

#define MUD_U48_SIZE (6U)
#define MUD_KEY_SIZE (32U)
#define MUD_MAC_SIZE (16U)

#define MUD_MSG(X)       ((X) & UINT64_C(1))
#define MUD_MSG_MARK(X)  ((X) | UINT64_C(1))
#define MUD_MSG_TC       (192) // CS6
#define MUD_MSG_SENT_MAX (3)
#define MUD_MSG_MIN_RTT  (100 * MUD_ONE_MSEC)

#define MUD_PKT_MIN_SIZE (MUD_U48_SIZE + MUD_MAC_SIZE)
#define MUD_PKT_MAX_SIZE (1500U)

#define MUD_MTU_MIN (1280U + MUD_PKT_MIN_SIZE)
#define MUD_MTU_MAX (1450U + MUD_PKT_MIN_SIZE)

#define MUD_TIME_BITS    (48)
#define MUD_TIME_MASK(X) ((X) & ((UINT64_C(1) << MUD_TIME_BITS) - 2))

#define MUD_WINDOW_TIMEOUT     ( 50 * MUD_ONE_MSEC)
#define MUD_KEYX_TIMEOUT       ( 60 * MUD_ONE_MIN)
#define MUD_KEYX_RESET_TIMEOUT (200 * MUD_ONE_MSEC)
#define MUD_TIME_TOLERANCE     ( 10 * MUD_ONE_MIN)

#define MUD_CTRL_SIZE (CMSG_SPACE(MUD_PKTINFO_SIZE) + \
                       CMSG_SPACE(sizeof(struct in6_pktinfo)) + \
                       CMSG_SPACE(sizeof(int)))

#define MUD_PATH_MAX (32U)

struct mud_crypto_opt {
    unsigned char *dst;
    const unsigned char *src;
    size_t size;
};

struct mud_crypto_key {
    struct {
        unsigned char key[MUD_KEY_SIZE];
        crypto_aead_aes256gcm_state state;
    } encrypt, decrypt;
    int aes;
};

struct mud_addr {
    union {
        unsigned char v6[16];
        struct {
            unsigned char zero[10];
            unsigned char ff[2];
            unsigned char v4[4];
        };
    };
    unsigned char port[2];
};

struct mud_msg {
    unsigned char sent[MUD_U48_SIZE];
    unsigned char state;
    struct mud_addr addr;
    unsigned char pub[MUD_PUB_SIZE];
    unsigned char aes;
    unsigned char fwd_dt[MUD_U48_SIZE];
    unsigned char fwd_send[MUD_U48_SIZE];
    unsigned char dt[MUD_U48_SIZE];
    unsigned char send[MUD_U48_SIZE];
    unsigned char recv[MUD_U48_SIZE];
 // unsigned char delay[MUD_U48_SIZE];
    unsigned char rate[MUD_U48_SIZE];
};

struct mud {
    int fd;
    uint64_t time_tolerance;
    uint64_t keyx_timeout;
    struct sockaddr_storage addr;
    struct mud_path *paths;
    unsigned count;
    struct {
        uint64_t time;
        unsigned char secret[crypto_scalarmult_SCALARBYTES];
        struct mud_public pub;
        struct mud_crypto_key private, last, next, current;
        int ready;
        int use_next;
        int aes;
    } crypto;
    uint64_t last_recv_time;
    size_t mtu;
    int msg_tc;
    struct {
        int set;
        struct sockaddr_storage addr;
    } peer;
    struct {
        struct {
            struct sockaddr_storage addr;
            uint64_t time;
        } decrypt, difftime, keyx;
    } bad;
    uint64_t window;
};

static int
mud_addr_is_v6(struct mud_addr *addr)
{
    static const unsigned char v4mapped[] = {
        [10] = 255,
        [11] = 255,
    };

    return memcmp(addr->v6, v4mapped, sizeof(v4mapped));
}

static int
mud_encrypt_opt(const struct mud_crypto_key *k,
                const struct mud_crypto_opt *c)
{
    if (k->aes) {
        unsigned char npub[crypto_aead_aes256gcm_NPUBBYTES] = {0};

        memcpy(npub, c->dst, MUD_U48_SIZE);

        return crypto_aead_aes256gcm_encrypt_afternm(
            c->dst + MUD_U48_SIZE,
            NULL,
            c->src,
            c->size,
            c->dst,
            MUD_U48_SIZE,
            NULL,
            npub,
            (const crypto_aead_aes256gcm_state *)&k->encrypt.state
        );
    } else {
        unsigned char npub[crypto_aead_chacha20poly1305_NPUBBYTES] = {0};

        memcpy(npub, c->dst, MUD_U48_SIZE);

        return crypto_aead_chacha20poly1305_encrypt(
            c->dst + MUD_U48_SIZE,
            NULL,
            c->src,
            c->size,
            c->dst,
            MUD_U48_SIZE,
            NULL,
            npub,
            k->encrypt.key
        );
    }
}

static int
mud_decrypt_opt(const struct mud_crypto_key *k,
                const struct mud_crypto_opt *c)
{
    if (k->aes) {
        unsigned char npub[crypto_aead_aes256gcm_NPUBBYTES] = {0};

        memcpy(npub, c->src, MUD_U48_SIZE);

        return crypto_aead_aes256gcm_decrypt_afternm(
            c->dst,
            NULL,
            NULL,
            c->src + MUD_U48_SIZE,
            c->size - MUD_U48_SIZE,
            c->src, MUD_U48_SIZE,
            npub,
            (const crypto_aead_aes256gcm_state *)&k->decrypt.state
        );
    } else {
        unsigned char npub[crypto_aead_chacha20poly1305_NPUBBYTES] = {0};

        memcpy(npub, c->src, MUD_U48_SIZE);

        return crypto_aead_chacha20poly1305_decrypt(
            c->dst,
            NULL,
            NULL,
            c->src + MUD_U48_SIZE,
            c->size - MUD_U48_SIZE,
            c->src, MUD_U48_SIZE,
            npub,
            k->decrypt.key
        );
    }
}

static void
mud_write48(unsigned char *dst, uint64_t src)
{
    dst[0] = (unsigned char)(UINT64_C(255) & (src));
    dst[1] = (unsigned char)(UINT64_C(255) & (src >> 8));
    dst[2] = (unsigned char)(UINT64_C(255) & (src >> 16));
    dst[3] = (unsigned char)(UINT64_C(255) & (src >> 24));
    dst[4] = (unsigned char)(UINT64_C(255) & (src >> 32));
    dst[5] = (unsigned char)(UINT64_C(255) & (src >> 40));
}

static uint64_t
mud_read48(const unsigned char *src)
{
    uint64_t ret = src[0];
    ret |= ((uint64_t)src[1]) << 8;
    ret |= ((uint64_t)src[2]) << 16;
    ret |= ((uint64_t)src[3]) << 24;
    ret |= ((uint64_t)src[4]) << 32;
    ret |= ((uint64_t)src[5]) << 40;
    return ret;
}

static uint64_t
mud_now(void)
{
    uint64_t now;
#if defined CLOCK_REALTIME
    struct timespec tv;
    clock_gettime(CLOCK_REALTIME, &tv);
    now = tv.tv_sec * MUD_ONE_SEC + tv.tv_nsec / MUD_ONE_MSEC;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    now = tv.tv_sec * MUD_ONE_SEC + tv.tv_usec;
#endif
    return MUD_TIME_MASK(now);
}

static uint64_t
mud_abs_diff(uint64_t a, uint64_t b)
{
    return (a >= b) ? a - b : b - a;
}

static int
mud_timeout(uint64_t now, uint64_t last, uint64_t timeout)
{
    return (!last) || (MUD_TIME_MASK(now - last) >= timeout);
}

static void
mud_unmapv4(struct sockaddr_storage *addr)
{
    if (addr->ss_family != AF_INET6)
        return;

    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;

    if (!IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
        return;

    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = sin6->sin6_port,
    };

    memcpy(&sin.sin_addr.s_addr,
           &sin6->sin6_addr.s6_addr[12],
           sizeof(sin.sin_addr.s_addr));

    memcpy(addr, &sin, sizeof(sin));
}

static struct mud_path *
mud_select_path(struct mud *mud, unsigned k)
{
    unsigned w = 0;
    struct mud_path *last = NULL;

    for (unsigned i = 0; i < mud->count; i++) {
        struct mud_path *path = &mud->paths[i];

        if (!path->window)
            continue;

        w += ((path->window << 16) + (mud->window >> 1)) / mud->window;
        last = path;

        if (k <= w)
            break;
    }

    return last;
}

static ssize_t
mud_send_path(struct mud *mud, struct mud_path *path, uint64_t now,
              void *data, size_t size, int tc, int flags)
{
    if (!size || !path)
        return 0;

    unsigned char ctrl[MUD_CTRL_SIZE] = {0};

    struct iovec iov = {
        .iov_base = data,
        .iov_len = size,
    };

    struct msghdr msg = {
        .msg_name = &path->addr,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl,
    };

    if (path->addr.ss_family == AF_INET) {
        msg.msg_namelen = sizeof(struct sockaddr_in);
        msg.msg_controllen = CMSG_SPACE(MUD_PKTINFO_SIZE) +
                             CMSG_SPACE(sizeof(int));

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type = MUD_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(MUD_PKTINFO_SIZE);

        memcpy(MUD_PKTINFO_DST(CMSG_DATA(cmsg)),
               &((struct sockaddr_in *)&path->local_addr)->sin_addr,
               sizeof(struct in_addr));

        cmsg = (struct cmsghdr *)((unsigned char *)cmsg +
                                  CMSG_SPACE(MUD_PKTINFO_SIZE));

        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type = IP_TOS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));

        memcpy(CMSG_DATA(cmsg), &tc, sizeof(int));
    }

    if (path->addr.ss_family == AF_INET6) {
        msg.msg_namelen = sizeof(struct sockaddr_in6);
        msg.msg_controllen = CMSG_SPACE(sizeof(struct in6_pktinfo)) +
                             CMSG_SPACE(sizeof(int));

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

        memcpy(&((struct in6_pktinfo *)CMSG_DATA(cmsg))->ipi6_addr,
               &((struct sockaddr_in6 *)&path->local_addr)->sin6_addr,
               sizeof(struct in6_addr));

        cmsg = (struct cmsghdr *)((unsigned char *)cmsg +
                                  CMSG_SPACE(sizeof(struct in6_pktinfo)));

        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_TCLASS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));

        memcpy(CMSG_DATA(cmsg), &tc, sizeof(int));
    }

    ssize_t ret = sendmsg(mud->fd, &msg, flags);

    path->send.total++;
    path->send.bytes += size;
    path->send.time = now;

    if (path->window > size) {
        mud->window -= size;
        path->window -= size;
    } else {
        mud->window -= path->window;
        path->window = 0;
    }

    return ret;
}

static int
mud_sso_int(int fd, int level, int optname, int opt)
{
    return setsockopt(fd, level, optname, &opt, sizeof(opt));
}

static int
mud_cmp_addr(struct sockaddr_storage *a, struct sockaddr_storage *b)
{
    if (a == b)
        return 0;

    if (a->ss_family != b->ss_family)
        return 1;

    if (a->ss_family == AF_INET) {
        struct sockaddr_in *_a = (struct sockaddr_in *)a;
        struct sockaddr_in *_b = (struct sockaddr_in *)b;

        return ((_a->sin_port != _b->sin_port) ||
                (memcmp(&_a->sin_addr, &_b->sin_addr,
                        sizeof(_a->sin_addr))));
    }

    if (a->ss_family == AF_INET6) {
        struct sockaddr_in6 *_a = (struct sockaddr_in6 *)a;
        struct sockaddr_in6 *_b = (struct sockaddr_in6 *)b;

        return ((_a->sin6_port != _b->sin6_port) ||
                (memcmp(&_a->sin6_addr, &_b->sin6_addr,
                        sizeof(_a->sin6_addr))));
    }

    return 1;
}

struct mud_path *
mud_get_paths(struct mud *mud, unsigned *ret_count)
{
    unsigned count = 0;

    if (!ret_count) {
        errno = EINVAL;
        return NULL;
    }

    for (unsigned i = 0; i < mud->count; i++) {
        struct mud_path *path = &mud->paths[i];

        if (path->state != MUD_EMPTY)
            count++;
    }

    size_t size = count * sizeof(struct mud_path);

    if (!size) {
        errno = 0;
        return NULL;
    }

    struct mud_path *paths = malloc(size);

    if (!paths)
        return NULL;

    count = 0;

    for (unsigned i = 0; i < mud->count; i++) {
        struct mud_path *path = &mud->paths[i];

        if (path->state != MUD_EMPTY)
            memcpy(&paths[count++], path, sizeof(struct mud_path));
    }

    *ret_count = count;

    return paths;
}

static void
mud_copy_port(struct sockaddr_storage *d, struct sockaddr_storage *s)
{
    uint16_t port = 0;

    switch (s->ss_family) {
    case AF_INET:
        port = ((struct sockaddr_in *)s)->sin_port;
        break;
    case AF_INET6:
        port = ((struct sockaddr_in6 *)s)->sin6_port;
        break;
    }

    switch (d->ss_family) {
    case AF_INET:
        ((struct sockaddr_in *)d)->sin_port = port;
        break;
    case AF_INET6:
        ((struct sockaddr_in6 *)d)->sin6_port = port;
        break;
    }
}

static void
mud_update_rate(struct mud *mud, struct mud_path *path, uint64_t rate)
{
    if (!rate)
        return;

    path->rate_tx = rate;
    path->window_size = (rate * MUD_WINDOW_TIMEOUT) / MUD_ONE_SEC;
}

static void
mud_reset_path(struct mud *mud, struct mud_path *path)
{
    path->window = 0;
    path->ok = 0;
    path->msg_sent = 0;
}

static struct mud_path *
mud_get_path(struct mud *mud, struct sockaddr_storage *local_addr,
             struct sockaddr_storage *addr, int create)
{
    if (local_addr->ss_family != addr->ss_family) {
        errno = EINVAL;
        return NULL;
    }

    mud_copy_port(local_addr, &mud->addr);

    for (unsigned i = 0; i < mud->count; i++) {
        struct mud_path *path = &mud->paths[i];

        if ((path->state != MUD_EMPTY) &&
            (!mud_cmp_addr(local_addr, &path->local_addr)) &&
            (!mud_cmp_addr(addr, &path->addr)))
            return path;
    }

    if (!create) {
        errno = 0;
        return NULL;
    }

    struct mud_path *path = NULL;

    for (unsigned i = 0; i < mud->count; i++) {
        if (mud->paths[i].state == MUD_EMPTY) {
            path = &mud->paths[i];
            break;
        }
    }

    if (!path) {
        if (mud->count == MUD_PATH_MAX) {
            errno = ENOMEM;
            return NULL;
        }

        struct mud_path *paths = realloc(mud->paths,
                (mud->count + 1) * sizeof(struct mud_path));

        if (!paths)
            return NULL;

        path = &paths[mud->count];

        mud->count++;
        mud->paths = paths;
    }

    memset(path, 0, sizeof(struct mud_path));

    memcpy(&path->local_addr, local_addr, sizeof(*local_addr));
    memcpy(&path->addr, addr, sizeof(*addr));

    path->state = MUD_UP;

    path->mtu.ok = MUD_MTU_MIN;
    path->mtu.min = MUD_MTU_MIN;
    path->mtu.max = MUD_MTU_MAX;
    path->mtu.probe = MUD_MTU_MAX;

    mud_reset_path(mud, path);

    return path;
}

static int
mud_ss_from_sa(struct sockaddr_storage *ss, struct sockaddr *sa)
{
    if (!ss || !sa) {
        errno = EINVAL;
        return -1;
    }

    switch (sa->sa_family) {
    case AF_INET:
        memcpy(ss, sa, sizeof(struct sockaddr_in));
        break;
    case AF_INET6:
        memcpy(ss, sa, sizeof(struct sockaddr_in6));
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    mud_unmapv4(ss);

    return 0;
}

int
mud_peer(struct mud *mud, struct sockaddr *peer)
{
    if (mud_ss_from_sa(&mud->peer.addr, peer))
        return -1;

    mud->peer.set = 1;

    return 0;
}

int
mud_get_key(struct mud *mud, unsigned char *key, size_t *size)
{
    if (!key || !size || (*size < MUD_KEY_SIZE)) {
        errno = EINVAL;
        return -1;
    }

    memcpy(key, mud->crypto.private.encrypt.key, MUD_KEY_SIZE);
    *size = MUD_KEY_SIZE;

    return 0;
}

int
mud_set_key(struct mud *mud, unsigned char *key, size_t size)
{
    if (key && (size < MUD_KEY_SIZE)) {
        errno = EINVAL;
        return -1;
    }

    unsigned char *enc = mud->crypto.private.encrypt.key;
    unsigned char *dec = mud->crypto.private.decrypt.key;

    if (key) {
        memcpy(enc, key, MUD_KEY_SIZE);
        sodium_memzero(key, size);
    } else {
        randombytes_buf(enc, MUD_KEY_SIZE);
    }

    memcpy(dec, enc, MUD_KEY_SIZE);

    mud->crypto.current = mud->crypto.private;
    mud->crypto.next = mud->crypto.private;
    mud->crypto.last = mud->crypto.private;

    return 0;
}

int
mud_set_tc(struct mud *mud, int tc)
{
    if (tc != (tc & 255)) {
        errno = EINVAL;
        return -1;
    }

    mud->msg_tc = tc;

    return 0;
}

static int
mud_set_msec(uint64_t *dst, unsigned long msec)
{
    if (!msec) {
        errno = EINVAL;
        return -1;
    }

    const uint64_t x = msec * MUD_ONE_MSEC;

    if ((x >> MUD_TIME_BITS) ||
        ((uint64_t)msec != x / MUD_ONE_MSEC)) {
        errno = ERANGE;
        return -1;
    }

    *dst = x;

    return 0;
}

int
mud_set_time_tolerance(struct mud *mud, unsigned long msec)
{
    return mud_set_msec(&mud->time_tolerance, msec);
}

int
mud_set_keyx_timeout(struct mud *mud, unsigned long msec)
{
    return mud_set_msec(&mud->keyx_timeout, msec);
}

int
mud_set_state(struct mud *mud, struct sockaddr *addr,
              enum mud_state state,
              unsigned long rate_tx,
              unsigned long rate_rx)
{
    if (!mud->peer.set || state > MUD_UP) {
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_storage local_addr;

    if (mud_ss_from_sa(&local_addr, addr))
        return -1;

    struct mud_path *path = mud_get_path(mud,
            &local_addr, &mud->peer.addr, state > MUD_DOWN);

    if (!path)
        return -1;

    if (rate_tx)
        mud_update_rate(mud, path, rate_tx);

    if (rate_rx)
        path->rate_rx = rate_rx;

    if (state && path->state != state) {
        path->state = state;
        mud_reset_path(mud, path); // XXX
    }

    return 0;
}

size_t
mud_get_mtu(struct mud *mud)
{
    return mud->mtu - MUD_PKT_MIN_SIZE;
}

void
mud_set_mtu(struct mud *mud, size_t mtu)
{
    mud->mtu = mtu + MUD_PKT_MIN_SIZE;
}

static int
mud_setup_socket(int fd, int v4, int v6)
{
    if ((mud_sso_int(fd, SOL_SOCKET, SO_REUSEADDR, 1)) ||
        (v4 && mud_sso_int(fd, IPPROTO_IP, MUD_PKTINFO, 1)) ||
        (v6 && mud_sso_int(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1)) ||
        (v6 && mud_sso_int(fd, IPPROTO_IPV6, IPV6_V6ONLY, !v4)))
        return -1;

#if defined MUD_DFRAG
    if (v4)
        mud_sso_int(fd, IPPROTO_IP, MUD_DFRAG, MUD_DFRAG_OPT);
#endif

    return 0;
}

static void
mud_keyx_set(struct mud *mud, unsigned char *key, unsigned char *secret,
             unsigned char *pub0, unsigned char *pub1)
{
    crypto_generichash_state state;

    crypto_generichash_init(&state, mud->crypto.private.encrypt.key,
                            MUD_KEY_SIZE, MUD_KEY_SIZE);

    crypto_generichash_update(&state, secret, crypto_scalarmult_BYTES);
    crypto_generichash_update(&state, pub0, MUD_PUB_SIZE);
    crypto_generichash_update(&state, pub1, MUD_PUB_SIZE);

    crypto_generichash_final(&state, key, MUD_KEY_SIZE);

    sodium_memzero(&state, sizeof(state));
}

static void
mud_keyx_reset(struct mud *mud)
{
    if (memcmp(&mud->crypto.current, &mud->crypto.private,
               sizeof(struct mud_crypto_key))) {
        mud->crypto.last = mud->crypto.current;
        mud->crypto.current = mud->crypto.private;
    }

    mud->crypto.ready = 1;
    mud->crypto.use_next = 0;
}

static int
mud_keyx(struct mud *mud, unsigned char *remote, int aes)
{
    unsigned char secret[crypto_scalarmult_BYTES];

    if (crypto_scalarmult(secret, mud->crypto.secret, remote))
        return 1;

    unsigned char *local = mud->crypto.pub.local;

    mud_keyx_set(mud, mud->crypto.next.encrypt.key, secret, remote, local);
    mud_keyx_set(mud, mud->crypto.next.decrypt.key, secret, local, remote);

    sodium_memzero(secret, sizeof(secret));

    memcpy(mud->crypto.pub.remote, remote, MUD_PUB_SIZE);

    mud->crypto.next.aes = mud->crypto.aes && aes;

    if (!mud->crypto.next.aes)
        return 0;

    crypto_aead_aes256gcm_beforenm((crypto_aead_aes256gcm_state *)
                                       &mud->crypto.next.encrypt.state,
                                   mud->crypto.next.encrypt.key);

    crypto_aead_aes256gcm_beforenm((crypto_aead_aes256gcm_state *)
                                       &mud->crypto.next.decrypt.state,
                                   mud->crypto.next.decrypt.key);

    return 0;
}

static void
mud_keyx_init(struct mud *mud, uint64_t now)
{
    if (!mud_timeout(now, mud->crypto.time, mud->keyx_timeout))
        return;

    mud->crypto.time = now;

    if (mud->crypto.ready)
        return;

    static const unsigned char test[crypto_scalarmult_BYTES] = {
        0x9b, 0xf4, 0x14, 0x90, 0x0f, 0xef, 0xf8, 0x2d, 0x11, 0x32, 0x6e,
        0x3d, 0x99, 0xce, 0x96, 0xb9, 0x4f, 0x79, 0x31, 0x01, 0xab, 0xaf,
        0xe3, 0x03, 0x59, 0x1a, 0xcd, 0xdd, 0xb0, 0xfb, 0xe3, 0x49
    };

    unsigned char tmp[crypto_scalarmult_BYTES];

    do {
        randombytes_buf(mud->crypto.secret, sizeof(mud->crypto.secret));
        crypto_scalarmult_base(mud->crypto.pub.local, mud->crypto.secret);
    } while (crypto_scalarmult(tmp, test, mud->crypto.pub.local));

    sodium_memzero(tmp, sizeof(tmp));

    mud->crypto.ready = 1;
}

int
mud_set_aes(struct mud *mud)
{
    if (!crypto_aead_aes256gcm_is_available()) {
        errno = ENOTSUP;
        return -1;
    }

    mud->crypto.aes = 1;

    return 0;
}

struct mud *
mud_create(struct sockaddr *addr)
{
    if (!addr)
        return NULL;

    int v4, v6;
    socklen_t addrlen = 0;

    switch (addr->sa_family) {
    case AF_INET:
        addrlen = sizeof(struct sockaddr_in);
        v4 = 1;
        v6 = 0;
        break;
    case AF_INET6:
        addrlen = sizeof(struct sockaddr_in6);
        v4 = MUD_V4V6;
        v6 = 1;
        break;
    default:
        return NULL;
    }

    if (sodium_init() == -1)
        return NULL;

    struct mud *mud = sodium_malloc(sizeof(struct mud));

    if (!mud)
        return NULL;

    memset(mud, 0, sizeof(struct mud));
    mud->fd = socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP);

    if ((mud->fd == -1) ||
        (mud_setup_socket(mud->fd, v4, v6)) ||
        (bind(mud->fd, addr, addrlen))) {
        mud_delete(mud);
        return NULL;
    }

    mud->time_tolerance = MUD_TIME_TOLERANCE;
    mud->keyx_timeout = MUD_KEYX_TIMEOUT;
    mud->msg_tc = MUD_MSG_TC;
    mud->mtu = MUD_MTU_MIN;

    memcpy(&mud->addr, addr, addrlen);

    return mud;
}

int
mud_get_fd(struct mud *mud)
{
    if (!mud)
        return -1;

    return mud->fd;
}

void
mud_delete(struct mud *mud)
{
    if (!mud)
        return;

    if (mud->paths)
        free(mud->paths);

    if (mud->fd >= 0)
        close(mud->fd);

    sodium_free(mud);
}

static int
mud_encrypt(struct mud *mud, uint64_t now,
            unsigned char *dst, size_t dst_size,
            const unsigned char *src, size_t src_size)
{
    const size_t size = src_size + MUD_PKT_MIN_SIZE;

    if (size > dst_size)
        return 0;

    const struct mud_crypto_opt opt = {
        .dst = dst,
        .src = src,
        .size = src_size,
    };

    mud_write48(dst, now);

    if (mud->crypto.use_next) {
        mud_encrypt_opt(&mud->crypto.next, &opt);
    } else {
        mud_encrypt_opt(&mud->crypto.current, &opt);
    }

    return size;
}

static int
mud_decrypt(struct mud *mud,
            unsigned char *dst, size_t dst_size,
            const unsigned char *src, size_t src_size)
{
    const size_t size = src_size - MUD_PKT_MIN_SIZE;

    if (size > dst_size)
        return 0;

    const struct mud_crypto_opt opt = {
        .dst = dst,
        .src = src,
        .size = src_size,
    };

    if (mud_decrypt_opt(&mud->crypto.current, &opt)) {
        if (!mud_decrypt_opt(&mud->crypto.next, &opt)) {
            mud->crypto.last = mud->crypto.current;
            mud->crypto.current = mud->crypto.next;
            mud->crypto.ready = 0;
            mud->crypto.use_next = 0;
        } else {
            if (mud_decrypt_opt(&mud->crypto.last, &opt) &&
                mud_decrypt_opt(&mud->crypto.private, &opt))
                return -1;
        }
    }

    return size;
}

static int
mud_localaddr(struct sockaddr_storage *addr, struct msghdr *msg)
{
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);

    for (; cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if ((cmsg->cmsg_level == IPPROTO_IP) &&
            (cmsg->cmsg_type == MUD_PKTINFO))
            break;
        if ((cmsg->cmsg_level == IPPROTO_IPV6) &&
            (cmsg->cmsg_type == IPV6_PKTINFO))
            break;
    }

    if (!cmsg)
        return 1;

    memset(addr, 0, sizeof(struct sockaddr_storage));

    if (cmsg->cmsg_level == IPPROTO_IP) {
        addr->ss_family = AF_INET;
        memcpy(&((struct sockaddr_in *)addr)->sin_addr,
               MUD_PKTINFO_SRC(CMSG_DATA(cmsg)),
               sizeof(struct in_addr));
    } else {
        addr->ss_family = AF_INET6;
        memcpy(&((struct sockaddr_in6 *)addr)->sin6_addr,
               &((struct in6_pktinfo *)CMSG_DATA(cmsg))->ipi6_addr,
               sizeof(struct in6_addr));
    }

    mud_unmapv4(addr);

    return 0;
}

static int
mud_send_msg(struct mud *mud, struct mud_path *path, uint64_t now,
                uint64_t sent, uint64_t fwd_send, uint64_t fwd_dt, size_t size)
{
    unsigned char dst[MUD_PKT_MAX_SIZE];
    unsigned char src[MUD_PKT_MAX_SIZE] = {0};
    struct mud_msg *msg = (struct mud_msg *)src;

    if (size < MUD_PKT_MIN_SIZE + sizeof(struct mud_msg))
        size = MUD_PKT_MIN_SIZE + sizeof(struct mud_msg);

    mud_write48(dst, MUD_MSG_MARK(now));
    mud_write48(msg->sent, sent);

    if (path->addr.ss_family == AF_INET) {
        msg->addr.ff[0] = 0xFF;
        msg->addr.ff[1] = 0xFF;
        memcpy(msg->addr.v4,
               &((struct sockaddr_in *)&path->addr)->sin_addr, 4);
        memcpy(msg->addr.port,
               &((struct sockaddr_in *)&path->addr)->sin_port, 2);
    } else if (path->addr.ss_family == AF_INET6) {
        memcpy(msg->addr.v6,
               &((struct sockaddr_in6 *)&path->addr)->sin6_addr, 16);
        memcpy(msg->addr.port,
               &((struct sockaddr_in6 *)&path->addr)->sin6_port, 2);
    } else {
        errno = EINVAL;
        return -1;
    }

    msg->state = (unsigned char)path->state;

    memcpy(msg->pub,
           mud->crypto.pub.local,
           sizeof(mud->crypto.pub.local));

    msg->aes = (unsigned char)mud->crypto.aes;

    uint64_t dt = 0;

    mud_write48(msg->send, path->send.bytes);
    mud_write48(msg->recv, path->recv.bytes);

    if (mud->peer.set) {
        if (sent) {
            dt = MUD_TIME_MASK(now - path->recv.msg_time);

            path->recv.bytes = 0;
            path->recv.msg_time = now;
        } else {
            dt = MUD_TIME_MASK(now - path->send.msg_time);

            path->send.bytes = 0;
            path->send.msg_time = now;

            if (path->msg_sent < MUD_MSG_SENT_MAX)
                path->msg_sent++;
        }
    } else {
        dt = MUD_TIME_MASK(now - path->recv.msg_time);

        path->send.bytes = 0;
        path->send.msg_time = now;

        path->recv.bytes = 0;
        path->recv.msg_time = now;

        if (path->msg_sent < MUD_MSG_SENT_MAX)
            path->msg_sent++;
    }

    mud_write48(msg->dt, dt);
    mud_write48(msg->fwd_dt, fwd_dt);
    mud_write48(msg->fwd_send, fwd_send);
    mud_write48(msg->rate, path->rate_rx);

    const struct mud_crypto_opt opt = {
        .dst = dst,
        .src = src,
        .size = size - MUD_PKT_MIN_SIZE,
    };

    mud_encrypt_opt(&mud->crypto.private, &opt);

    return mud_send_path(mud, path, now, dst, size,
                         mud->msg_tc, sent ? MSG_CONFIRM : 0);
}

static int
mud_decrypt_msg(struct mud *mud,
                   unsigned char *dst, size_t dst_size,
                   const unsigned char *src, size_t src_size)
{
    const size_t size = src_size - MUD_PKT_MIN_SIZE;

    if (size < sizeof(struct mud_msg))
        return 0;

    const struct mud_crypto_opt opt = {
        .dst = dst,
        .src = src,
        .size = src_size,
    };

    if (mud_decrypt_opt(&mud->crypto.private, &opt))
        return -1;

    return size;
}

static void
mud_update_stat(struct mud_stat *stat, const uint64_t val)
{
    if (stat->setup) {
        const uint64_t var = mud_abs_diff(stat->val, val);
        stat->var = ((stat->var << 1) + stat->var + var) >> 2;
        stat->val = ((stat->val << 3) - stat->val + val) >> 3;
    } else {
        stat->setup = 1;
        stat->var = val >> 1;
        stat->val = val;
    }
}

static void
mud_ss_from_packet(struct sockaddr_storage *ss, struct mud_msg *pkt)
{
    if (mud_addr_is_v6(&pkt->addr)) {
        ss->ss_family = AF_INET6;
        memcpy(&((struct sockaddr_in6 *)ss)->sin6_addr, pkt->addr.v6, 16);
        memcpy(&((struct sockaddr_in6 *)ss)->sin6_port, pkt->addr.port, 2);
    } else {
        ss->ss_family = AF_INET;
        memcpy(&((struct sockaddr_in *)ss)->sin_addr, pkt->addr.v4, 4);
        memcpy(&((struct sockaddr_in *)ss)->sin_port, pkt->addr.port, 2);
    }
}

static void
mud_update_window(struct mud *mud, struct mud_path *path,
                  uint64_t now, uint64_t sent,
                  uint64_t send_dt, uint64_t send_bytes,
                  uint64_t recv_dt, uint64_t recv_bytes)
{
    // TODO
}

static void
mud_recv_msg(struct mud *mud, struct mud_path *path,
                uint64_t now, uint64_t sent,
                unsigned char *data, size_t size)
{
    struct mud_msg *msg = (struct mud_msg *)data;

    mud_ss_from_packet(&path->r_addr, msg);

    const uint64_t peer_sent = mud_read48(msg->sent);

    if (peer_sent) {
        path->mtu.min = size + 1;
        path->mtu.ok = size;

        if (!path->ok) {
            path->mtu.max = MUD_MTU_MAX;
            path->mtu.probe = MUD_MTU_MAX;
        } else {
            path->mtu.probe = (path->mtu.min + path->mtu.max) >> 1;
        }

        mud_update_stat(&path->rtt, MUD_TIME_MASK(now - peer_sent));
        mud_update_window(mud, path, now, sent,
                mud_read48(msg->fwd_dt),
                mud_read48(msg->fwd_send),
                mud_read48(msg->dt),
                mud_read48(msg->recv));

        path->msg_sent = 0;
        path->ok = 1;
    } else {
        mud_keyx_init(mud, now);
        path->state = (enum mud_state)msg->state;
        mud_update_rate(mud, path, mud_read48(msg->rate));
    }

    const int rem = memcmp(msg->pub,
                           mud->crypto.pub.remote,
                           MUD_PUB_SIZE);

    const int loc = memcmp(path->pub.local,
                           mud->crypto.pub.local,
                           MUD_PUB_SIZE);

    if (rem || loc) {
        if (mud_keyx(mud, msg->pub, msg->aes)) {
            mud->bad.keyx.addr = path->addr;
            mud->bad.keyx.time = now;
            return;
        }

        if (!mud->peer.set) {
            for (unsigned i = 0; i < mud->count; i++) {
                if (mud->paths[i].state == MUD_EMPTY)
                    continue;

                if (memcmp(mud->paths[i].pub.remote,
                           path->pub.remote,
                           MUD_PUB_SIZE) &&
                    memcmp(mud->paths[i].pub.remote,
                           msg->pub,
                           MUD_PUB_SIZE))
                    mud->paths[i].state = MUD_EMPTY;
            }
        }

        path->pub = mud->crypto.pub;
    } else {
        mud->crypto.use_next = 1;
    }

    const uint64_t fwd_dt = mud_read48(msg->dt);
    const uint64_t fwd_send = mud_read48(msg->send);

    if (!peer_sent || mud->peer.set)
        mud_send_msg(mud, path, now, sent, fwd_send, fwd_dt, size);
}

int
mud_recv(struct mud *mud, void *data, size_t size)
{
    unsigned char packet[MUD_PKT_MAX_SIZE];

    struct iovec iov = {
        .iov_base = packet,
        .iov_len = sizeof(packet),
    };

    struct sockaddr_storage addr;
    unsigned char ctrl[MUD_CTRL_SIZE];

    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl,
        .msg_controllen = sizeof(ctrl),
    };

    const ssize_t packet_size = recvmsg(mud->fd, &msg, 0);

    if (packet_size == (ssize_t)-1)
        return -1;

    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        (packet_size <= (ssize_t)MUD_PKT_MIN_SIZE))
        return 0;

    const uint64_t now = mud_now();
    const uint64_t send_time = mud_read48(packet);

    mud_unmapv4(&addr);

    if ((MUD_TIME_MASK(now - send_time) > mud->time_tolerance) &&
        (MUD_TIME_MASK(send_time - now) > mud->time_tolerance)) {
        mud->bad.difftime.addr = addr;
        mud->bad.difftime.time = now;
        return 0;
    }

    const int ret = MUD_MSG(send_time)
                  ? mud_decrypt_msg(mud, data, size, packet, packet_size)
                  : mud_decrypt(mud, data, size, packet, packet_size);

    if (ret <= 0) {
        mud->bad.decrypt.addr = addr;
        mud->bad.decrypt.time = now;
        return 0;
    }

    struct sockaddr_storage local_addr;

    if (mud_localaddr(&local_addr, &msg))
        return 0;

    struct mud_path *path = mud_get_path(mud, &local_addr, &addr, 1);

    if (!path)
        return 0;

    if (path->state <= MUD_DOWN)
        return 0;

    if (MUD_MSG(send_time))
        mud_recv_msg(mud, path, now, send_time, data, packet_size);

    path->recv.total++;
    path->recv.time = now;
    path->recv.bytes += packet_size;

    mud->last_recv_time = now;

    return MUD_MSG(send_time) ? 0 : ret;
}

static uint64_t
mud_path_timeout(struct mud_path *path)
{
    if (!path->rtt.setup)
        return MUD_MSG_MIN_RTT;

    const uint64_t rtt = (path->rtt.val + path->rtt.var) << 1;

    return rtt > MUD_MSG_MIN_RTT ? rtt : MUD_MSG_MIN_RTT;
}

static void
mud_update(struct mud *mud, uint64_t now)
{
    if (mud->peer.set) {
        mud_keyx_init(mud, now);

        if (mud_timeout(now, mud->last_recv_time, MUD_KEYX_RESET_TIMEOUT))
            mud_keyx_reset(mud);
    }

    uint64_t window = 0;
    size_t mtu = 0;

    for (unsigned i = 0; i < mud->count; i++) {
        struct mud_path *path = &mud->paths[i];

        if (path->state <= MUD_DOWN)
            continue;

        if (mud->peer.set) {
            if (path->msg_sent >= MUD_MSG_SENT_MAX) {
                if (path->mtu.probe == MUD_MTU_MIN) {
                    mud_reset_path(mud, path);
                } else {
                    if (path->mtu.ok == path->mtu.probe) {
                        path->mtu.min = MUD_MTU_MIN;
                        path->mtu.ok = MUD_MTU_MIN;
                        mud_reset_path(mud, path);
                    } else {
                        path->msg_sent = 0;
                    }
                    path->mtu.max = path->mtu.probe - 1;
                    path->mtu.probe = (path->mtu.min + path->mtu.max) >> 1;
                }
            }
        } else {
            if ((path->msg_sent >= MUD_MSG_SENT_MAX) ||
                (path->recv.time &&
                 mud->last_recv_time > path->recv.time + MUD_ONE_SEC)) {
                mud_reset_path(mud, path);
            }
        }

        if (path->ok) {
            if (!mtu || mtu > path->mtu.ok) {
                mtu = path->mtu.ok;
            }
            if (mud_timeout(now, path->window_time, MUD_WINDOW_TIMEOUT)) {
                path->window = path->window_size;
                path->window_time = now;
            }
        } else {
            path->window = 0;
        }

        if (mud->peer.set) {
            if (mud_timeout(now, path->send.msg_time, mud_path_timeout(path)))
                mud_send_msg(mud, path, now, 0, 0, 0, path->mtu.probe);
        }

        window += path->window;
    }

    mud->window = window;
    mud->mtu = mtu ?: MUD_MTU_MIN;
}

long
mud_send_wait(struct mud *mud)
{
    const uint64_t now = mud_now();

    mud_update(mud, now);

    if (mud->window)
        return 0;

    long dt = MUD_ONE_SEC - 1;
    unsigned not_down = 0;

    for (unsigned i = 0; i < mud->count; i++) {
        struct mud_path *path = &mud->paths[i];

        if (path->state <= MUD_DOWN)
            continue;

        not_down++;

        if (!path->ok)
            continue;

        uint64_t elapsed = MUD_TIME_MASK(now - path->window_time);

        if (elapsed >= MUD_WINDOW_TIMEOUT)
            continue;

        uint64_t new_dt = MUD_WINDOW_TIMEOUT - elapsed;

        if ((uint64_t)dt > new_dt)
            dt = (long)new_dt;
    }

    return not_down ? dt : -1;
}

int
mud_send(struct mud *mud, const void *data, size_t size, unsigned tc)
{
    if (!size)
        return 0;

    if (!mud->window) {
        errno = EAGAIN;
        return -1;
    }

    unsigned char packet[MUD_PKT_MAX_SIZE];
    const uint64_t now = mud_now();
    const int packet_size = mud_encrypt(mud, now,
                                        packet, sizeof(packet),
                                        data, size);
    if (!packet_size) {
        errno = EMSGSIZE;
        return -1;
    }

    unsigned k = tc >> 8;

    if (!k) {
        const unsigned a = packet[packet_size - 1];
        const unsigned b = packet[packet_size - 2];
        k = (a << 8) | b;
    } else {
        k--;
    }

    return mud_send_path(mud, mud_select_path(mud, k),
                         now, packet, packet_size, tc & 255, 0);
}
