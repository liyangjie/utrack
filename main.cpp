/*
utrack is a very small an efficient BitTorrent tracker
Copyright (C) 2010  Arvid Norberg

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "hash.hpp"
#include "swarm.hpp"
#include "messages.hpp"
#include "endian.hpp"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// if this is true, we allow peers to set which IP
// they will announce as. This is off by default since
// it allows for spoofing
bool allow_alternate_ip = false;

int interval = 1800;

int listen_port = 8080;

int num_threads = 4;

int socket_buffer_size = 5 * 1024 * 1024;

// set to true when we're shutting down
volatile bool quit = false;

// this is the UDP socket we accept tracker announces to
int udp_socket = -1;

// partial SHA-1 hash of the secret key, combined with
// source IP and port if forms the connection-id
SHA_CTX secret;

// read-write lock for the swarm hash table
pthread_rwlock_t swarm_mutex;

// the address and port we receive packets on, and also
// for sending responses (although over a separate socket)
sockaddr_in bind_addr;

// the swarm hash table. The read lock must be held
// when making lookups, the write lock must be held when
// adding or remiving swarms
typedef hash_map<sha1_hash, swarm*, sha1_hash_fun> swarm_map_t;
swarm_map_t swarms;

// round-robin for timing out peers
swarm_map_t::iterator next_to_purge;

// stats counters
uint32_t connects = 0;
uint32_t announces = 0;
uint32_t scrapes = 0;
uint32_t errors = 0;
uint32_t bytes_out = 0;
uint32_t bytes_in = 0;

void gen_secret_digest(sockaddr_in const* from, char* digest)
{
	SHA_CTX ctx = secret;
	SHA1_Update(&ctx, (char*)&from->sin_addr, sizeof(from->sin_addr));
	SHA1_Update(&ctx, (char*)&from->sin_port, sizeof(from->sin_port));
	SHA1_Final((unsigned char*)digest, &ctx);
}

uint64_t generate_connection_id(sockaddr_in const* from)
{
//#error add an option to use an insecure, cheap method
	char digest[20];
	gen_secret_digest(from, digest);
	uint64_t ret;
	memcpy((char*)&ret, digest, sizeof(ret));
	return ret;
}

bool verify_connection_id(uint64_t conn_id, sockaddr_in* from)
{
	char digest[20];
	gen_secret_digest(from, digest);
	return memcmp((char*)&conn_id, digest, sizeof(conn_id)) == 0;
}

// send a packet and retry on EINTR
bool respond(int socket, char const* buf, int len, sockaddr const* to, socklen_t tolen)
{
	ssize_t ret = 0;
retry_send:
	ret = sendto(socket, buf, len, MSG_NOSIGNAL, to, tolen);
	if (ret == -1)
	{
		if (errno == EINTR) goto retry_send;
		fprintf(stderr, "sendto failed (%d): %s\n", errno, strerror(errno));
		return 1;
	}
	__sync_fetch_and_add(&bytes_out, ret);
	return 0;
}

void* tracker_thread(void* arg)
{
	sigset_t sig;
	sigfillset(&sig);
	int r = pthread_sigmask(SIG_BLOCK, &sig, NULL);
	if (r == -1)
	{
		fprintf(stderr, "pthread_sigmask failed (%d): %s\n", errno, strerror(errno));
	}

	// this is the socket this thread will use to send responses on
	// to mitigate congestion on the receive socket
	int send_socket = socket(PF_INET, SOCK_DGRAM, 0);
	if (send_socket < 0)
	{
		fprintf(stderr, "failed to open send socket (%d): %s\n"
			, errno, strerror(errno));
		return 0;
	}

	int one = 1;
	if (setsockopt(send_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
	{
		fprintf(stderr, "failed to set SO_REUSEADDR on socket (%d): %s\n"
			, errno, strerror(errno));
	}

#ifdef SO_REUSEPORT
	if (setsockopt(send_socket, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0)
	{
		fprintf(stderr, "failed to set SO_REUSEPORT on socket (%d): %s\n"
			, errno, strerror(errno));
	}
#endif

	if (bind(send_socket, (sockaddr*)&bind_addr, sizeof(bind_addr)) < 0)
	{
		fprintf(stderr, "failed to bind send socket to port %d (%d): %s\n"
			, ntohs(bind_addr.sin_port), errno, strerror(errno));
		close(send_socket);
		return 0;
	}

	int opt = socket_buffer_size;
	if (setsockopt(send_socket, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt)) < 0)
	{
		fprintf(stderr, "failed to set socket send buffer size (%d): %s\n"
			, errno, strerror(errno));
	}

	sockaddr_in from;
	// use uint64_t to make the buffer properly aligned
	uint64_t buffer[1500/8];

	for (;;)
	{
		socklen_t fromlen = sizeof(from);
		int size = recvfrom(udp_socket, (char*)buffer, sizeof(buffer), 0
			, (sockaddr*)&from, &fromlen);
		if (size == -1)
		{
			if (errno == EINTR) continue;
			fprintf(stderr, "recvfrom failed (%d): %s\n", errno, strerror(errno));
			break;
		}
		__sync_fetch_and_add(&bytes_in, size);

//		printf("received message from: %x port: %d size: %d\n"
//			, from.sin_addr.s_addr, ntohs(from.sin_port), size);

		if (size < 16)
		{
			// log incorrect packet
			continue;
		}

		udp_announce_message* hdr = (udp_announce_message*)buffer;

		switch (ntohl(hdr->action))
		{
			case action_connect:
			{
				if (be64toh(hdr->connection_id) != 0x41727101980LL)
				{
					__sync_fetch_and_add(&errors, 1);
					printf("invalid connection ID for connect message\n");
					// log error
					continue;
				}
				udp_connect_response resp;
				resp.action = htonl(action_connect);
				resp.connection_id = generate_connection_id(&from);
				resp.transaction_id = hdr->transaction_id;
				__sync_fetch_and_add(&connects, 1);
				if (respond(send_socket, (char*)&resp, 16, (sockaddr*)&from, fromlen))
					return 0;
				break;
			}
			case action_announce:
			{
				if (!verify_connection_id(hdr->connection_id, &from))
				{
					printf("invalid connection ID\n");
					__sync_fetch_and_add(&errors, 1);
					// log error
					continue;
				}
				// technically the announce message should
				// be 100 bytes, but uTorrent doesn't seem to send
				// the extension field at the end
				if (size < 98)
				{
					printf("announce packet too short. Expected 100, got %d\n", size);
					__sync_fetch_and_add(&errors, 1);
					// log incorrect packet
					continue;
				}
				__sync_fetch_and_add(&announces, 1);

				if (!allow_alternate_ip || hdr->ip == 0)
					hdr->ip = from.sin_addr.s_addr;

				pthread_rwlock_rdlock(&swarm_mutex);
				swarm_map_t::iterator i = swarms.find(hdr->hash);
				swarm* s = 0;
				if (i != swarms.end())
				{
					s = i->second;
				}
				pthread_rwlock_unlock(&swarm_mutex);

				if (s == 0)
				{
					// the swarm doesn't exist, we need to add it
					s = new swarm;

					pthread_rwlock_wrlock(&swarm_mutex);
					swarms.insert(std::make_pair(hdr->hash, s));
					pthread_rwlock_unlock(&swarm_mutex);
				}

				char* buf;
				int len;
				iovec iov[2];
				msghdr msg;
				udp_announce_response resp;

				resp.action = htonl(action_announce);
				resp.transaction_id = hdr->transaction_id;
				resp.interval = htonl(1680 + rand() * 240 / RAND_MAX);

				msg.msg_name = (void*)&from;
				msg.msg_namelen = fromlen;
				msg.msg_iov = iov;
				msg.msg_iovlen = 2;
				msg.msg_control = 0;
				msg.msg_controllen = 0;
				msg.msg_flags = 0;

				iov[0].iov_base = &resp;
				iov[0].iov_len = 20;

				swarm_lock l(*s);
				s->announce(hdr, &buf, &len, &resp.downloaders, &resp.seeds);
				resp.downloaders = htonl(resp.downloaders);
				resp.seeds = htonl(resp.seeds);

				iov[1].iov_base = buf;
				iov[1].iov_len = len;
//				fprintf(stderr, "sending %d bytes payload\n", len);

				// silly loop just to deal with the potential EINTR
				do
				{
					r = sendmsg(send_socket, &msg, MSG_NOSIGNAL);
					if (r == -1)
					{
						if (errno == EINTR) continue;
						fprintf(stderr, "sendmsg failed (%d): %s\n", errno, strerror(errno));
						return 0;
					}
					__sync_fetch_and_add(&bytes_out, r);
				} while (false);
				break;
			}
			case action_scrape:
			{
				if (!verify_connection_id(hdr->connection_id, &from))
				{
					printf("invalid connection ID for connect message\n");
					__sync_fetch_and_add(&errors, 1);
					// log error
					continue;
				}
				if (size < 16 + 20)
				{
					printf("scrape packet too short. Expected 36, got %d\n", size);
					__sync_fetch_and_add(&errors, 1);
					// log error
					continue;
				}

				__sync_fetch_and_add(&scrapes, 1);

				// if someone sent a very large scrape request, only
				// respond to the first ones. We don't want to lock
				// too many swarms for just one response
				int num_hashes = (std::min)((size - 16) / 20, int(max_scrape_responses));

				udp_scrape_message* req = (udp_scrape_message*)buffer;

				udp_scrape_response resp;
				resp.action = htonl(action_scrape);
				resp.transaction_id = hdr->transaction_id;

				pthread_rwlock_rdlock(&swarm_mutex);
				for (int i = 0; i < num_hashes; ++i)
				{
					swarm_map_t::iterator j = swarms.find(req->hash[i]);
					if (j != swarms.end())
					{
						swarm* s = j->second;
						swarm_lock l(*s);
						s->scrape(&resp.data[i].seeds, &resp.data[i].download_count
							, &resp.data[i].downloaders);
						resp.data[i].seeds = htonl(resp.data[i].seeds);
						resp.data[i].download_count = htonl(resp.data[i].download_count);
						resp.data[i].downloaders = htonl(resp.data[i].downloaders);
					}
				}
				pthread_rwlock_unlock(&swarm_mutex);

				if (respond(send_socket, (char*)&resp, 8 + num_hashes * 12, (sockaddr*)&from, fromlen))
					return 0;

				break;
			}
			default:
				printf("unknown action %d\n", ntohl(hdr->action));
				__sync_fetch_and_add(&errors, 1);
				break;
		}
	}

	close(send_socket);

	return 0;
}

void sigint(int s)
{
	quit = true;
}

int main(int argc, char* argv[])
{
	// TODO: TEMP!
//	allow_alternate_ip = true;

	next_to_purge = swarms.begin();

	// initialize secret key which the connection-ids are built off of
	uint64_t secret_key = 0;
	for (int i = 0; i < sizeof(secret_key); ++i)
	{
		secret_key <<= 8;
		secret_key ^= rand();
	}
	SHA1_Init(&secret);
	SHA1_Update(&secret, &secret_key, sizeof(secret_key));

	int r = pthread_rwlock_init(&swarm_mutex, 0);
	if (r != 0)
	{
		fprintf(stderr, "pthread_rwlock_init failed (%d): %s\n", r, strerror(r));
		return EXIT_FAILURE;
	}

	udp_socket = socket(PF_INET, SOCK_DGRAM, 0);
	if (udp_socket < 0)
	{
		fprintf(stderr, "failed to open socket (%d): %s\n"
			, errno, strerror(errno));
		return EXIT_FAILURE;
	}

	int opt = socket_buffer_size;
	r = setsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(opt));
	if (r == -1)
	{
		fprintf(stderr, "failed to set socket receive buffer size (%d): %s\n"
			, errno, strerror(errno));
	}

	int one = 1;
	if (setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
	{
		fprintf(stderr, "failed to set SO_REUSEADDR on socket (%d): %s\n"
			, errno, strerror(errno));
	}

#ifdef SO_REUSEPORT
	if (setsockopt(udp_socket, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0)
	{
		fprintf(stderr, "failed to set SO_REUSEPORT on socket (%d): %s\n"
			, errno, strerror(errno));
	}
#endif

	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = INADDR_ANY;
	bind_addr.sin_port = htons(listen_port);
	r = bind(udp_socket, (sockaddr*)&bind_addr, sizeof(bind_addr));
	if (r < 0)
	{
		fprintf(stderr, "failed to bind socket to port %d (%d): %s\n"
			, listen_port, errno, strerror(errno));
		return EXIT_FAILURE;
	}
	fprintf(stderr, "listening on UDP port %d\n", listen_port);
	
	pthread_t* threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads);
	if (threads == NULL)
	{
		fprintf(stderr, "failed allocate thread list (no memory)\n");
		return EXIT_FAILURE;
	}

	// create threads
	for (int i = 0; i < num_threads; ++i)
	{
		printf("starting thread %d\n", i);
		r = pthread_create(&threads[i], NULL, &tracker_thread, 0);
		if (r != 0)
		{
			fprintf(stderr, "failed to create thread (%d): %s\n", r, strerror(r));
			return EXIT_FAILURE;
		}
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &sigint;
	r = sigaction(SIGINT, &sa, 0);
	if (r == -1)
	{
		fprintf(stderr, "sigaction failed (%d): %s\n", errno, strerror(errno));
		quit = true;
	}
	r = sigaction(SIGTERM, &sa, 0);
	if (r == -1)
	{
		fprintf(stderr, "sigaction failed (%d): %s\n", errno, strerror(errno));
		quit = true;
	}
	if (!quit) fprintf(stderr, "send SIGINT or SIGTERM to quit\n");

	while (!quit)
	{
		usleep(60000000);
		uint32_t last_connects = connects;
		uint32_t last_announces = announces;
		uint32_t last_scrapes = scrapes;
		uint32_t last_errors = errors;
		uint32_t last_bytes_in = bytes_in;
		uint32_t last_bytes_out = bytes_out;
		__sync_fetch_and_sub(&connects, last_connects);
		__sync_fetch_and_sub(&announces, last_announces);
		__sync_fetch_and_sub(&scrapes, last_scrapes);
		__sync_fetch_and_sub(&errors, last_errors);
		__sync_fetch_and_sub(&bytes_in, last_bytes_in);
		__sync_fetch_and_sub(&bytes_out, last_bytes_out);
		printf("c: %u a: %u s: %u e: %u in: %u kB out: %u kB\n"
			, last_connects, last_announces, last_scrapes, last_errors
			, last_bytes_in / 1000, last_bytes_out / 1000);

		time_t now = time(0);

		pthread_rwlock_rdlock(&swarm_mutex);
		if (next_to_purge == swarms.end() && swarms.size() > 0)
			next_to_purge = swarms.begin();

		int num_to_purge = (std::min)(int(swarms.size()), 20);

		for (int i = 0; i < num_to_purge; ++i)
		{
			if (next_to_purge == swarms.end()) next_to_purge = swarms.begin();
			else ++next_to_purge;
			if (next_to_purge == swarms.end()) break;

			swarm* s = next_to_purge->second;
			swarm_lock l(*s);
			s->purge_stale(now);
		}
		pthread_rwlock_unlock(&swarm_mutex);
	}

	close(udp_socket);

	for (int i = 0; i < num_threads; ++i)
	{
		void* retval = 0;
		pthread_join(threads[i], &retval);
		printf("thread %d terminated\n", i);
	}

	free(threads);

	pthread_rwlock_destroy(&swarm_mutex);

	return EXIT_SUCCESS;
}
