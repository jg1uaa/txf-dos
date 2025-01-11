// SPDX-License-Identifier: WTFPL

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <endian.h>
#include <i86.h>

#define MAGIC_SEND	0x53454e44	// "SEND"
#define MAGIC_RCVD	0x72637664	// "rcvd"
#define FILENAME_LEN	20
#define MIN_BLOCKSIZE	1
#define MAX_BLOCKSIZE	1024
#define MAX_FILE_SIZE	0x7fffffff

extern char *optarg;

static int serdev = -1;
static bool rtscts = false;
static int blocksize = MAX_BLOCKSIZE;

static int portarg;

_Packed struct txf_header {
	uint32_t magic;
	uint32_t filesize;		// big endian
	char filename[FILENAME_LEN];
	char filename_term;		// must be zero
	char unused[3];
};

struct txf_workingset {
	void *(*init)(char *arg);
	int (*process)(int d, void *handle);
	void (*finish)(void *handle);
};

struct txf_tx_workarea {
	FILE *fp;
	long size;
	struct txf_header h;
};

static void check_abort(void)
{
	union REGS r;
	unsigned short cflag;

	/* check key status: r.w.cflag not working, use inline assembler */
	_asm {
		mov ah, 1
		int 16h
		pushf
		pop ax
		mov cflag, ax
	}
	if (cflag & 0x0040)
		return;

	/* if ESC pressed, abort */
	r.h.ah = 0;
	int86(0x16, &r, &r);
	if (r.h.al == 0x1b)
		abort();
}

static uint16_t get_port_status(int p)
{
	union REGS r;

	r.w.dx = p;
	r.h.ah = 0x03;
	int86(0x14, &r, &r);
	
	return r.w.ax;
}

static int write_serial(int p, char c)
{
	union REGS r;

	/* wait for ready */
	while (!(get_port_status(p) & 0x4000))
		check_abort();

	r.w.dx = p;
	r.h.al = c;
	r.h.ah = 0x01;
	int86(0x14, &r, &r);

	return (r.h.ah & 0x80) ? -1 : 1;
}

static int read_serial(int p)
{
	union REGS r;

	/* wait for ready */
	while (!(get_port_status(p) & 0x0100))
		check_abort();

	r.w.dx = p;
	r.h.ah = 0x02;
	int86(0x14, &r, &r);

	return (r.h.ah & 0x80) ? -1 : r.h.al;
}

static int send_block(int d, void *buf, int size)
{
	int pos;
	char *p = (char *)buf;

	for (pos = 0; pos < size; pos++) {
		if (write_serial(d, *p++) < 0)
			break;
	}

	return pos;
}

static int recv_block(int d, void *buf, int size)
{
	int pos, c;
	char *p = (char *)buf;

	for (pos = 0; pos < size; pos++) {
		if ((c = read_serial(d)) < 0)
			break;
		*p++ = c;
	}

	return pos;
}

static char *get_filename(char *filename)
{
#define DELIMITER	'\\'
	
	int i, len;
	char *p;

	/* find the last delimiter character */
	len = strlen(filename);
	for (i = len - 1; i >= 0; i--) {
		if (filename[i] == DELIMITER)
			break;
	}

	/* filename starts after delimiter */
	p = filename + i + 1;

	/* check file name length */
	len = strlen(p);
	return (len < 1 || len > FILENAME_LEN) ? NULL : p;
}

static void wait_magic(int fd, void *header, uint32_t magic)
{
	int i;
	unsigned char *p = header;

	for (i = 0; i < sizeof(magic); i++) {
		if (recv_block(fd, &p[i], sizeof(*p)) != sizeof(*p) ||
		    p[i] != (unsigned char)(magic >> (8 * (3 - i))))
			i = 0;
	}
}

static void *rx_init(char *arg)
{
	/* do nothing */
	return rx_init;
}

static int rx_process(int fd, void *handle)
{
	FILE *fp;
	long i, size, remain;
	struct txf_header h;
	char *fn, buf[MAX_BLOCKSIZE];
	int rv = -1;

	/* wait for magic */
	wait_magic(fd, &h, MAGIC_SEND);

	/* receive header */
	remain = sizeof(h) - sizeof(h.magic);
	if (recv_block(fd, &h.filesize, remain) < remain) {
		printf("rx_process: recv_block (header)\n");
		goto fin0;
	}

	h.filename_term = '\0';
	size = be32toh(h.filesize);
	if ((fn = get_filename(h.filename)) == NULL) {
		printf("rx_process: invalid file name\n");
		goto fin0;
	}

	printf("%s, %d byte\n", fn, size);

	/* receive file */
	if ((fp = fopen(fn, "wb")) == NULL) {
		printf("rx_process: fopen\n");
		goto fin0;
	}

	for (i = 0; i < size; i += blocksize) {
		remain = size - i;
		if (remain > blocksize)
			remain = blocksize;

		if (recv_block(fd, buf, remain) < remain) {
			printf("rx_process: recv_block (data)\n");
			goto fin1;
		}

		if (fwrite(buf, remain, 1, fp) < 1) {
			printf("rx_process: fwrite\n");
			goto fin1;
		}

		fflush(fp);
	}

	/* send ack */
	h.magic = htobe32(MAGIC_RCVD);
	if (send_block(fd, &h, sizeof(h)) < sizeof(h)) {
		printf("rx_process: send_block (ack)\n");
		goto fin1;
	}

	rv = 0;
fin1:
	fclose(fp);
fin0:
	return rv;
}

static void rx_finish(void *handle)
{
	/* do nothing */
}

static void *tx_init(char *filename)
{
	struct  txf_tx_workarea *wk;
	FILE *fp;
	long size;
	char *fn;

	wk = malloc(sizeof(*wk));
	if (wk == NULL) {
		printf("tx_init: malloc\n");
		goto fin0;
	}

	/* file open */
	if ((fn = get_filename(filename)) == NULL) {
		printf("tx_init: invalid file name\n");
		goto fin1;
	}

	if ((fp = fopen(filename, "rb")) == NULL) {
		printf("tx_init: fopen\n");
		goto fin1;
	}

	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size < 0 /* || size > MAX_FILE_SIZE */) {
		printf("tx_init: invalid file size\n");
		goto fin2;
	}

	/* store file information to workarea */
	wk->fp = fp;
	wk->size = size;

	memset(&wk->h, 0, sizeof(wk->h));
	wk->h.magic = htobe32(MAGIC_SEND);
	wk->h.filesize = htobe32(size);
	strcpy(wk->h.filename, fn);

	printf("%s, %ld byte\n", fn, size);
	goto fin0;

fin2:
	fclose(fp);
fin1:
	free(wk);
	wk = NULL;
fin0:
	return wk;
}

static int tx_process(int d, void *handle)
{
	struct  txf_tx_workarea *wk = handle;
	long i, remain;
	struct txf_header h;
	char buf[MAX_BLOCKSIZE];
	int rv = -1;

	/* send header */
	if (send_block(d, &wk->h, sizeof(wk->h)) < sizeof(wk->h)) {
		printf("tx_process: send_block (header)\n");
		goto fin0;
	}

	/* send file */
	for (i = 0; i < wk->size; i += blocksize) {
		remain = wk->size - i;
		if (remain > blocksize)
			remain = blocksize;

		if (fread(buf, remain, 1, wk->fp) < 1) {
			printf("tx_process: fread\n");
			goto fin0;
		}

		if (send_block(d, buf, remain) < remain) {
			printf("tx_process: send_block (data)\n");
			goto fin0;
		}
	}

	/* wait for magic */
	wait_magic(d, &h, MAGIC_RCVD);

	/* receive ack */
	remain = sizeof(h) - sizeof(h.magic);
	if (recv_block(d, &h.filesize, remain) < remain) {
		printf("tx_process: recv_block (ack)\n");
		goto fin0;
	}

	rv = 0;
fin0:
	return rv;
}

static void tx_finish(void *handle)
{
	struct txf_tx_workarea *wk = handle;

	fclose(wk->fp);
	free(handle);
}

static int xfer(int fd, char *arg, struct txf_workingset *work)
{
	void *handle;
	int rv = -1;

	if ((handle = (*work->init)(arg)) == NULL) {
		printf("xfer: init\n");
		goto fin0;
	}

	if ((*work->process)(fd, handle)) {
		printf("xfer: process\n");
		goto fin1;
	}

	rv = 0;
fin1:
	(*work->finish)(handle);
fin0:
	return rv;
}

static int get_speed(int speed)
{
	int v;
	switch (speed) {
	case 110: v = 0; break;
	case 150: v = 1; break;
	case 300: v = 2; break;
	case 600: v = 3; break;
	case 1200: v = 4; break;
	case 2400: v = 5; break;
	case 4800: v = 6; break;
	case 9600: v = 7; break;
	default: return -1;
	}

	/* no parity, one stop bit, 8bits */
	return (v << 5) | 0x03;
}

static int open_serial(int p, int s)
{
	union REGS r;
	char c;

	r.w.dx = p;
	r.w.ax = get_speed(s); // ah = 0;
	int86(0x14, &r, &r);

	// send dummy data for stable
	c = 0;
	send_block(p, &c, sizeof(c));

	return p;
}

static int do_main(char *tx_file)
{
	int fd_ser, ret = -1;
	struct txf_workingset rx_set = {rx_init, rx_process, rx_finish};
	struct txf_workingset tx_set = {tx_init, tx_process, tx_finish};
	struct txf_workingset *set;

	if (tx_file == NULL) {
		printf("* receive\n");
		set = &rx_set;
	} else {
		printf("* transmit\n");
		set = &tx_set;
	}

	fd_ser = open_serial(serdev, portarg);
	if (fd_ser < 0) {
		printf("device open error\n");
		goto fin0;
	}

	xfer(fd_ser, tx_file, set);
	ret = 0;

	close(fd_ser);
fin0:
	return ret;
}

int main(int argc, char *argv[])
{
	int ch, v;
	char *tx_file = NULL;

	while ((ch = getopt(argc, argv, "s:l:cf:b:w:")) != -1) {
		switch (ch) {
		case 's':
			portarg = atoi(optarg);
			break;
		case 'l':
			if (!strcasecmp(optarg, "com1")) serdev = 0;
			else if (!strcasecmp(optarg, "com2")) serdev = 1;
			else if (!strcasecmp(optarg, "com3")) serdev = 2;
			else if (!strcasecmp(optarg, "com4")) serdev = 3;
			break;
		case 'c':
			rtscts = true;
			break;
		case 'f':
			tx_file = optarg;
			break;
		case 'b':
			v = atoi(optarg);
			if (v < MIN_BLOCKSIZE) v = MIN_BLOCKSIZE;
			if (v > MAX_BLOCKSIZE) v = MAX_BLOCKSIZE;
			blocksize = v;
			break;
		}
	}

	if (serdev < 0 || get_speed(portarg) < 0) {
		printf("usage:	%s -s [speed] -l [com1-4]\n", argv[0]);
		printf("	%s -s [speed] -l [com1-4] "
		       "-f [filename]\n", argv[0]);
		goto fin0;
	}

	do_main(tx_file);

fin0:
	return 0;
}
