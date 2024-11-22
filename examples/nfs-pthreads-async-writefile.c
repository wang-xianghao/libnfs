/* 
   Copyright (C) by Ronnie Sahlberg <ronniesahlberg@gmail.com> 2024
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/
    
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef AROS
#include "aros_compat.h"
#endif


#ifdef WIN32
#include <win32/win32_compat.h>
#pragma comment(lib, "ws2_32.lib")
WSADATA wsaData;
#else
#include <sys/stat.h>
#include <string.h>
#endif
 
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "libnfs.h"
#include "libnfs-raw.h"
#include "libnfs-raw-mount.h"

#define NUM_CONTEXTS 4
#define CHUNK_SIZE (10*1024*1024)

void usage(void)
{
	fprintf(stderr, "Usage: nfs-pthreads-writefile <src> <dst>\n");
	fprintf(stderr, "<src> is a local file, <dst> is an nfs URL.\n");
	exit(0);
}

struct write_data {
	struct nfs_context *nfs;
	char *ptr;
	struct nfsfh *nfsfh;
	uint64_t offset;
	ssize_t len;
};

struct write_cb_data {
	atomic_int calls_in_flight;
	int status;
};

void write_async_cb(int status, struct nfs_context *nfs,
		    void *data, void *private_data)
{
	struct write_cb_data *write_cb_data = private_data;

	if (status < 0) {
		fprintf(stderr, "pwrite failed with \"%s\"\n", (char *)data);
		status = -EIO;
	}
	atomic_fetch_sub_explicit(&write_cb_data->calls_in_flight, 1, memory_order_relaxed);
}

/*
 * Thread that is created to write an up to CHUNK_SIZE prt of the file.
 */
static void *nfs_write_thread(void *arg)
{
	struct write_data *wd = arg;
	ssize_t count;
	struct write_cb_data write_cb_data;
	struct timespec ts;
	
	write_cb_data.status = 0;
	write_cb_data.calls_in_flight = 0;
	while (wd->len) {
		count = 65536;
		if (count > wd->len) {
			count = wd->len;
		}
		atomic_fetch_add_explicit(&write_cb_data.calls_in_flight, 1, memory_order_relaxed);
		if (nfs_pwrite_async(wd->nfs, wd->nfsfh,
				     wd->ptr + wd->offset,
				     count, wd->offset,
				     write_async_cb, &write_cb_data) < 0) {
			fprintf(stderr, "Failed to write to nfs file. %s\n", nfs_get_error(wd->nfs));
			exit(1);
		}
		wd->offset += count;
		wd->len -= count;
	}
	while(write_cb_data.calls_in_flight) {
		ts.tv_sec = 0;
		ts.tv_nsec = 1000000;
		nanosleep(&ts, NULL);
	}
	if (write_cb_data.status) {
		fprintf(stderr, "Oh, no, something went wrong\n");
		exit(1);
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	int i, num_threads;
	int fd = -1;
	struct nfs_context *nfs[NUM_CONTEXTS] = {NULL,};
	struct nfs_url *url = NULL;
	struct stat st;
        pthread_t *write_threads;
        struct write_data *wd;
	struct nfsfh *nfsfh = NULL;
	char *ptr;

#ifdef WIN32
	if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
		printf("Failed to start Winsock2\n");
		return 10;
	}
#endif

#ifdef AROS
	aros_init_socket();
#endif

	if (argc != 3) {
		usage();
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open source file %s\n", argv[1]);
		exit(1);
	}
	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "failed to stat(%s)\n", argv[1]);
		exit(10);
	}
	ptr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
		fprintf(stderr, "failed to mmap file. %s\n", strerror(errno));
		exit(1);
	}
	/*
	 * Create NUM_CONTEXT number of connections to the server, each
	 * having its own service thread to perform all I/O to/from the
	 * socket.
	 */
	for (i = 0; i < NUM_CONTEXTS; i++) {
		nfs[i] = nfs_init_context();
		if (nfs[i] == NULL) {
			fprintf(stderr, "failed to init context\n");
			exit(1);
		}
		/*
		 * Bump the number of xid hashes so we don't have to spend
		 * too much time scanning the linked list for each hash bucket
		 * if we have very many threads doing i/o concurrently.
		 */
		nfs_set_hash_size(nfs[i], 50);
		if (url) {
			nfs_destroy_url(url);
		}
		url = nfs_parse_url_full(nfs[i], argv[2]);
		if (url == NULL) {
			fprintf(stderr, "%s\n", nfs_get_error(nfs[0]));
			exit(1);
		}

		if (nfs_mount(nfs[i], url->server, url->path) != 0) {
			fprintf(stderr, "Failed to mount nfs share : %s\n",
				nfs_get_error(nfs[i]));
			exit(1);
		}

		/*
		 * We just need to open the file once and can then just
		 * use the handle from all the other contexts too.
		 * This is good because creat()/O_CREAT|O_TRUNC is not
		 * an atomic operation in NFS.
		 */
		if (i == 0) {
			if (nfs_open(nfs[0], url->file, O_WRONLY|O_CREAT|O_TRUNC, &nfsfh) < 0) {
				fprintf(stderr, "Failed to open nfs file %s. %s\n",
					url->file,
					nfs_get_error(nfs[0]));
				exit(1);
			}
		}
		  
		/*
		 * Before we can use multithreading we must initialize and
		 * start the service thread.
		 */
		if (nfs_mt_service_thread_start(nfs[i])) {
			fprintf(stderr, "failed to start service thread\n");
			exit(10);
		}
		printf("Service thread #%d is active. Ready to do I/O\n", i);
	}

	/*
	 * Create threads to write the file. Each thread will write a
	 * CHUNK_SIZE portion of the file.
	 */
	printf("Size of file:%s is %d bytes\n", argv[1], st.st_size);
	num_threads = (st.st_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
	printf("Need %d threads to write %d bytes each\n", num_threads, CHUNK_SIZE);
        if ((write_threads = malloc(sizeof(pthread_t) * num_threads)) == NULL) {
		fprintf(stderr, "Failed to allocated stat_thread\n");
                exit(10);
        }
        if ((wd = malloc(sizeof(struct write_data) * num_threads)) == NULL) {
		fprintf(stderr, "Failed to allocated write_data\n");
                exit(10);
        }
        for (i = 0; i < num_threads; i++) {
                wd[i].nfs = nfs[i % NUM_CONTEXTS];
		wd[i].ptr = ptr;
                wd[i].nfsfh = nfsfh;
		wd[i].offset = i * CHUNK_SIZE;
		wd[i].len = st.st_size - wd[i].offset;
		if (wd[i].len > CHUNK_SIZE) {
			wd[i].len = CHUNK_SIZE;
		}
		  
                if (pthread_create(&write_threads[i], NULL,
                                   &nfs_write_thread, &wd[i])) {
                        printf("Failed to create stat thread %d\n", i);
                        exit(10);
                }
	}

	/*
	 * Wait for all threads to complete
	 */
        for (i = 0; i < num_threads; i++) {
                pthread_join(write_threads[i], NULL);
        }

	/*
	 * Closing the files
	 */
	nfs_close(nfs[0], nfsfh);
	close(fd);

        printf("closing service threads\n");
	for (i = 0; i < NUM_CONTEXTS; i++) {
		nfs_mt_service_thread_stop(nfs[i]);
		nfs_destroy_context(nfs[i]);
	}
	if (url) {
		nfs_destroy_url(url);
	}
	free(wd);
	free(write_threads);
	munmap(ptr, st.st_size);
	return 0;
}
