/*********************************************************************
 * Copyright (C) 2003 Tord Lindstrom (pukko@home.se)
 * This file is subject to the terms and conditions of the PS2Link License.
 * See the file LICENSE in the main directory of this distribution for more
 * details.
 */

#ifndef _NETFIO_H_
#define _NETFIO_H_

int pko_file_serv(void *arg);
int pko_recv_bytes(int fd, char *buf, int bytes);
int pko_accept_pkt(int fd, char *buf, int len, int pkt_type);
int pko_open_file(char *path, int flags);
int pko_close_file(int fd);
int pko_read_file(int fd, char *buf, int length);
int pko_write_file(int fd, char *buf, int length);
int pko_lseek_file(int fd, unsigned int offset, int whence);
void pko_close_socket(void);
void pko_close_fsys(void);

/*
 * Don't want printfs to broadcast in case more than 1 ps2 on the same network, so at
 * connect time, the remote PC's IP is stored here and used as destination for printfs.
 */
extern unsigned int remote_pc_addr;

#endif