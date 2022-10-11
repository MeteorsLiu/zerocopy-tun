#include "tun.h"
struct block_desc {
	uint32_t version;
	uint32_t offset_to_priv;
	struct tpacket_hdr_v1 h1;
};

struct ring {
	struct iovec *rd;
	uint8_t *map;
	struct tpacket_req3 req;
};
struct epoll_context {
    struct epoll_event events[2];
    int epollfd;
};

struct Context {
    struct epoll_context epoll;
    struct ring ring;
    int tunfd;
    int ringfd;
};


volatile sig_atomic_t exit_signal_received;
static void signal_handler(int sig)
{
    signal(sig, SIG_DFL);
    exit_signal_received = 1;
}



static int setup_socket(struct ring *ring, char *netdev)
{
	int err, i, fd, v = TPACKET_V3;
	struct sockaddr_ll ll;
	unsigned int blocksiz = 1 << 22, framesiz = 1 << 11;
	unsigned int blocknum = 64;

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		perror("socket");
		exit(1);
	}

	err = setsockopt(fd, SOL_PACKET, PACKET_VERSION, &v, sizeof(v));
	if (err < 0) {
		perror("setsockopt");
		exit(1);
	}

	memset(&ring->req, 0, sizeof(ring->req));
	ring->req.tp_block_size = blocksiz;
	ring->req.tp_frame_size = framesiz;
	ring->req.tp_block_nr = blocknum;
	ring->req.tp_frame_nr = (blocksiz * blocknum) / framesiz;
	ring->req.tp_retire_blk_tov = 60;
	ring->req.tp_feature_req_word = TP_FT_REQ_FILL_RXHASH;

	err = setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &ring->req,
			 sizeof(ring->req));
	if (err < 0) {
		perror("setsockopt");
		exit(1);
	}

	ring->map = mmap(NULL, ring->req.tp_block_size * ring->req.tp_block_nr,
			 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, fd, 0);
	if (ring->map == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	ring->rd = malloc(ring->req.tp_block_nr * sizeof(*ring->rd));
	assert(ring->rd);
	for (i = 0; i < ring->req.tp_block_nr; ++i) {
		ring->rd[i].iov_base = ring->map + (i * ring->req.tp_block_size);
		ring->rd[i].iov_len = ring->req.tp_block_size;
	}

	memset(&ll, 0, sizeof(ll));
	ll.sll_family = PF_PACKET;
	ll.sll_protocol = htons(ETH_P_ALL);
	ll.sll_ifindex = if_nametoindex(netdev);
	ll.sll_hatype = 0;
	ll.sll_pkttype = 0;
	ll.sll_halen = 0;

	err = bind(fd, (struct sockaddr *) &ll, sizeof(ll));
	if (err < 0) {
		perror("bind");
		exit(1);
	}

	return fd;
}
static void display(struct tpacket3_hdr *ppd)
{
	//struct ethhdr *eth = (struct ethhdr *) ((uint8_t *) ppd + ppd->tp_mac);
	struct iphdr *ip = (struct iphdr *) (ppd + 8);

	printf("%d.%d.%d.%d\n", NIPQUAD(ip->saddr));


	//printf("rxhash: 0x%x\n", ppd->hv1.tp_rxhash);
}
static void walk_block(struct block_desc *pbd, const int block_num)
{
	int num_pkts = pbd->h1.num_pkts, i;
	unsigned long bytes = 0;
	struct tpacket3_hdr *ppd;

	ppd = (struct tpacket3_hdr *) ((uint8_t *) pbd +
				       pbd->h1.offset_to_first_pkt);
	for (i = 0; i < num_pkts; ++i) {
		
		display(ppd);

		ppd = (struct tpacket3_hdr *) ((uint8_t *) ppd +
					       ppd->tp_next_offset);
	}

}

static void flush_block(struct block_desc *pbd)
{
	pbd->h1.block_status = TP_STATUS_KERNEL;
}

static void teardown_socket(struct Context *ctx)
{
	munmap(ctx->ring.map, ctx->ring.req.tp_block_size * ctx->ring.req.tp_block_nr);
	free(ctx->ring.rd);
	close(ctx->ringfd);

}
int event_add(struct Context *ctx) 
{
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ctx->ringfd;
    if (epoll_ctl(ctx->epoll.epollfd, EPOLL_CTL_ADD, ctx->ringfd, &ev) == -1) {
        perror("epoll_ctl: ring fd");
        return -1;
    }
    memset(&ev, 0, sizeof(struct epoll_event));
    ev.events = EPOLLOUT;
    ev.data.fd = ctx->tunfd;
    if (epoll_ctl(ctx->epoll.epollfd, EPOLL_CTL_ADD, ctx->tunfd, &ev) == -1) {
        perror("epoll_ctl: tun fd");
        return -1;
    }
}

int tun_create(char if_name[IFNAMSIZ], const char *wanted_name)
{
    struct ifreq ifr;
    int          fd;
    int          err;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "tun module not present. See https://sk.tl/2RdReigK\n");
        return -1;
    }
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", wanted_name == NULL ? "" : wanted_name);
    if (ioctl(fd, TUNSETIFF, &ifr) != 0) {
        err = errno;
        (void) close(fd);
        errno = err;
        return -1;
    }
    snprintf(if_name, IFNAMSIZ, "%s", ifr.ifr_name);
    return fd;
}

void usage()
{
    puts("usage:\n"
        "./tun TUN-NAME\n"
    );
}
int main(int argc, char **argp)
{
    unsigned int block_num = 0, blocks = 64;
    int nfds;
	struct block_desc *pbd;
	struct tpacket_stats_v3 stats;
    struct Context ctx;
    char if_name[IFNAMSIZ];
    if (argc < 1) {
        usage();
        goto exit;
    }
	ctx.epoll.epollfd = epoll_create1(0);
    ctx.tunfd =  tun_create(if_name, argp[1]);
    printf("CreateTun: %s\n", if_name);
    memset(&ctx.ring, 0, sizeof(struct ring));
	ctx.ringfd = setup_socket(&ctx.ring, if_name);

	if (ctx.tunfd == -1 || ctx.ringfd == -1 || ctx.epoll.epollfd == -1) {
		printf("fd error");
		goto exit;
	}
    event_add(&ctx);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    while (exit_signal_received != 1) {
			nfds = epoll_wait(ctx.epoll.epollfd, ctx.epoll.events, 2, -1);
            if (nfds == -1) {
				printf("no waiter");
				continue;	
			}
            for (int n = 0; n < nfds; ++n) {
                if (ctx.epoll.events[n].data.fd == ctx.ringfd) {
					pbd = (struct block_desc *) ctx.ring.rd[block_num].iov_base;

					if ((pbd->h1.block_status & TP_STATUS_USER) == 0) 
					{
						continue;
					}
					walk_block(pbd, block_num);
					flush_block(pbd);
					block_num = (block_num + 1) % blocks;
				}
            }
			
    }
    teardown_socket(&ctx);
    close(ctx.tunfd);
	close(ctx.epoll.epollfd);
exit:
    return 0;
}