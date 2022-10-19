#include "tun.h"


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
	Buf buf;
	int tunfd;
	int pipefd[2];
};

volatile sig_atomic_t exit_signal_received;
static void signal_handler(int sig)
{
	signal(sig, SIG_DFL);
	exit_signal_received = 1;
}

int event_add(struct Context *ctx)
{
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = ctx->tunfd;
	if (epoll_ctl(ctx->epoll.epollfd, EPOLL_CTL_ADD, ctx->tunfd, &ev) == -1)
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

void close_all(struct Context *ctx)
{
	close(ctx->tunfd);
	close(ctx->epoll.epollfd);
	close(ctx->pipefd[0]);
	close(ctx->pipefd[1]);
}
int main(int argc, char **argp)
{
	int nfds;
	struct Context ctx;
	char if_name[IFNAMSIZ];
	if (argc < 1)
	{
		goto exit;
	}
	if (pipe(ctx.pipefd) < 0)
	{
		perror("pipe error");
		goto exit;
	}
	ctx.epoll.epollfd = epoll_create1(0);
	ctx.tunfd = tun_create(if_name, argp[1]);
	printf("CreateTun: %s\n", if_name);


	if (ctx.tunfd == -1 || ctx.epoll.epollfd == -1)
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

		if (ctx.epoll.events[0].data.fd == ctx.tunfd)
		{
			if (splice(ctx.tunfd, NULL, ctx.pipefd[1], NULL, 1500, 0) < 0) {
                perror("splice");
            }
		}
	}
	close_all(&ctx);
exit:
	return 0;
}