#include "tun.h"
#include "rand.h"
#include <time.h>
struct block_desc
{
	uint32_t version;
	uint32_t offset_to_priv;
	struct tpacket_hdr_v1 h1;
};

struct ring
{
	struct iovec *rd;
	uint8_t *map;
	struct tpacket_req3 req;
};
struct epoll_context
{
	struct epoll_event events[1];
	int epollfd;
};
typedef struct __attribute__((aligned(16))) Buf_
{
	unsigned char len[2];
	unsigned char data[MAX_PACKET_LEN];
	size_t pos;
} Buf;

struct Context
{
	struct epoll_context epoll;
	struct ring ring;
	Buf buf;
	int tunfd;
	int ringfd;
};

volatile sig_atomic_t exit_signal_received;
static void signal_handler(int sig)
{
	signal(sig, SIG_DFL);
	exit_signal_received = 1;
}

int rand_range(int from, int to)
{
	unsigned int randint;
	srand_sse((unsigned)time(NULL) + from + to);
	rand_sse(&randint, 16);
	return from + ((int)randint % (to - from + 1));
}

static int setup_socket(struct ring *ring, char *netdev)
{
	int err, i, fd, v = TPACKET_V3;
	struct sockaddr_ll ll;
	unsigned int blocksiz = 1 << 22, framesiz = 1 << 11;
	unsigned int blocknum = 64;

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0)
	{
		perror("socket");
		exit(1);
	}

	err = setsockopt(fd, SOL_PACKET, PACKET_VERSION, &v, sizeof(v));
	if (err < 0)
	{
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
	if (err < 0)
	{
		perror("setsockopt");
		exit(1);
	}

	ring->map = mmap(NULL, ring->req.tp_block_size * ring->req.tp_block_nr,
					 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, fd, 0);
	if (ring->map == MAP_FAILED)
	{
		perror("mmap");
		exit(1);
	}

	ring->rd = malloc(ring->req.tp_block_nr * sizeof(*ring->rd));
	assert(ring->rd);
	for (i = 0; i < ring->req.tp_block_nr; ++i)
	{
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

	err = bind(fd, (struct sockaddr *)&ll, sizeof(ll));
	if (err < 0)
	{
		perror("bind");
		exit(1);
	}

	return fd;
}
static void copy_to_buf(struct Context *ctx, struct tpacket3_hdr *ppd)
{
	memset(ctx->buf.data, 0, sizeof(ctx->buf.data));

	memcpy(ctx->buf.data, (uint8_t *)ppd + ppd->tp_mac, (size_t)ppd->tp_len);

	int len, padding_len, size;
	unsigned int randint;
	uint16_t binlen;
	if (ppd->tp_len / 2 < 16)
	{
		len = rand_range(16, 17);
	}
	else
	{
		len = rand_range(16, ppd->tp_len / 2);
	}

	if (1500 - ppd->tp_len - len < 0)
	{
		len = rand_range(16, 1500 - ppd->tp_len);
	}
	if (len > 100)
	{
		size = sizeof(unsigned int);
	}
	else
	{
		size = sizeof(uint16_t);
	}
	for (int i = 0; i < len; i += size)
	{
		srand_sse((unsigned)time(NULL) + i);
		if (size > sizeof(uint16_t)) 
		{
			rand_sse(&randint, 0);
			randint = endian_swap64(randint);
			memcpy(ctx->buf.data + ppd->tp_len + i, &randint, sizeof(unsigned int));
		}
		else 
		{
			rand_sse(&randint, 16);
			binlen = endian_swap16((uint16_t)randint);
			memcpy(ctx->buf.data + ppd->tp_len + i, &binlen, sizeof(uint16_t));
		}
	
		
	}

	binlen = endian_swap16((uint16_t)ppd->tp_len);
	memcpy(ctx->buf.len, &binlen, 2);

	printf("Rand Int With Buf Size: %d, len : %d\n", ppd->tp_len + len, len);
}

static void echo(struct Context *ctx, struct tpacket3_hdr *ppd)
{
	unsigned char ip[4];
	unsigned char buffer[1500];
	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer, (uint8_t *)ppd + ppd->tp_mac, (size_t)ppd->tp_len);
	memcpy(ip, &buffer[12], 4);
    memcpy(&buffer[12], &buffer[16], 4);
    memcpy(&buffer[16], ip, 4);

    buffer[20] = 0;
    *((unsigned short *)&buffer[22]) += 8;
	while (write(ctx->tunfd, buffer, (size_t)ppd->tp_len) < (ssize_t) 0 && errno == EINTR &&
           !exit_signal_received)
		;

}
static void walk_block(struct Context *ctx, struct block_desc *pbd)
{
	int num_pkts = pbd->h1.num_pkts, i;
	struct tpacket3_hdr *ppd;

	ppd = (struct tpacket3_hdr *)((uint8_t *)pbd +
								  pbd->h1.offset_to_first_pkt);
	for (i = 0; i < num_pkts; ++i)
	{

		echo(ctx, ppd);
		ppd = (struct tpacket3_hdr *)((uint8_t *)ppd +
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
	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = ctx->ringfd;
	if (epoll_ctl(ctx->epoll.epollfd, EPOLL_CTL_ADD, ctx->ringfd, &ev) == -1)
	{
		perror("epoll_ctl: ring fd");
		return -1;
	}
}

int tun_create(char if_name[IFNAMSIZ], const char *wanted_name)
{
	struct ifreq ifr;
	int fd;
	int err;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd == -1)
	{
		fprintf(stderr, "tun module not present. See https://sk.tl/2RdReigK\n");
		return -1;
	}
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", wanted_name == NULL ? "" : wanted_name);
	if (ioctl(fd, TUNSETIFF, &ifr) != 0)
	{
		err = errno;
		(void)close(fd);
		errno = err;
		return -1;
	}
	snprintf(if_name, IFNAMSIZ, "%s", ifr.ifr_name);
	return fd;
}

void usage()
{
	puts("usage:\n"
		 "./tun TUN-NAME\n");
}
int main(int argc, char **argp)
{
	unsigned int block_num = 0, blocks = 64;
	int nfds;
	struct block_desc *pbd;
	struct tpacket_stats_v3 stats;
	struct Context ctx;
	char if_name[IFNAMSIZ];
	if (argc < 1)
	{
		usage();
		goto exit;
	}
	ctx.epoll.epollfd = epoll_create1(0);
	ctx.tunfd = tun_create(if_name, argp[1]);
	printf("CreateTun: %s\n", if_name);
	memset(&ctx.ring, 0, sizeof(struct ring));
	ctx.ringfd = setup_socket(&ctx.ring, if_name);

	if (ctx.tunfd == -1 || ctx.ringfd == -1 || ctx.epoll.epollfd == -1)
	{
		printf("fd error");
		goto exit;
	}
	event_add(&ctx);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	while (exit_signal_received != 1)
	{
		nfds = epoll_wait(ctx.epoll.epollfd, ctx.epoll.events, 1, -1);
		if (nfds == -1)
		{
			printf("no waiter");
			continue;
		}

		if (ctx.epoll.events[0].data.fd == ctx.ringfd)
		{
			pbd = (struct block_desc *)ctx.ring.rd[block_num].iov_base;

			if ((pbd->h1.block_status & TP_STATUS_USER) == 0)
			{
				continue;
			}
			walk_block(&ctx, pbd);
			flush_block(pbd);
			block_num = (block_num + 1) % blocks;
		}
	}
	teardown_socket(&ctx);
	close(ctx.tunfd);
	close(ctx.epoll.epollfd);
exit:
	return 0;
}