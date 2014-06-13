 /* This file is in the public domain. */

#define _POSIX_C_SOURCE 199309

#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <tox/tox.h>

/* Bootstrap info */
static const char *BS_IP = "192.254.75.98";
static const uint16_t BS_PORT = 33445;
static const char *BS_KEY = 
	"951C88B7E75C867418ACDB5D273821372BB5BD652740BCDF623A4FA293E75D2F";

/* Old Groupbot info */
static const char *GB_ADDR =
	"56A1ADE4B65B86BCD51CC73E2CD4E542179F47959FE3E0E21B4B0ACDADE51855D34D34D37CB5";
static int32_t gb_id;
static bool send_invite;

/* Our info */
static const char *NICK = "NGB";
static const char *STATUS = "Send me a message with the word \"invite\"";
static uint8_t ADDR[2*TOX_FRIEND_ADDRESS_SIZE];

static const char *GROUPFILE = "groupbot.data";
static const char *GROUPFILE_TMP = "groupbot.data.tmp";

__attribute__((__format__ (__printf__, 1, 2)))
static _Noreturn void error(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

__attribute__((__format__ (__printf__, 1, 2)))
static void info(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
}

static void parse_hex(uint8_t *dst, size_t dst_len, const char *src, size_t src_len) {
	size_t i, j;
	uint8_t val;

	assert(src_len == 2*dst_len);
	for (i = 0; i < dst_len; i++) {
		dst[i] = 0;
		for (j = 0; j < 2; j++) {
			val = (uint8_t)src[2*i + j];
			dst[i] <<= 4*j;
			switch (val) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				dst[i] |= val - '0';
				break;
			case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
				dst[i] |= val - 'A' + 10;
				break;
			default:
				error("ERROR: Invalid hex character %c\n", val);
			}
		}
	}
}

static void format_hex(uint8_t *dst, size_t dst_len, uint8_t *src, size_t src_len) {
	size_t i, j;
	unsigned int val;

	assert(dst_len == 2*src_len);
	for (i = 0; i < src_len; i++) {
		for (j = 0; j < 2; j++) {
			val = src[i];
			val = (val >> 4*(1-j)) & 0xF;
			if (val < 10) {
				dst[2*i + j] = (uint8_t)('0' + val);
			} else {
				dst[2*i + j] = (uint8_t)('A' + val - 10);
			}
		}
	}
}

static void on_status_message(Tox *tox, int32_t id, uint8_t *msg, uint16_t len,
		void *userdata) {
	(void)tox;
	(void)msg;
	(void)len;
	(void)userdata;

	if (id == gb_id) {
		info("INFO: Groupbot status message\n");
		send_invite = true;
	}
}

static void on_group_invite(Tox *tox, int32_t friend, uint8_t *group, void *userdata) {
	int ret;

	(void)userdata;

	if (tox_count_chatlist(tox) > 0) {
		return;
	}

	if (friend == gb_id) {
		info("INFO: Invited by Groupbot\n");
		ret = tox_join_groupchat(tox, friend, group);
		if (ret == -1) {
			error("ERROR: Failed to join group\n");
		}
	}
}

static void on_friend_message(Tox *tox, int32_t friend, uint8_t *msg, uint16_t len,
		void *userdata) {
	int group_id;
	uint32_t ret;
	int ret2;

	(void)msg;
	(void)len;
	(void)userdata;

	ret = tox_get_chatlist(tox, &group_id, 1);
	if (ret != 1) {
		info("WARN: Groupchat not found\n");
		return;
	}
	info("INFO: Inviting\n");
	ret2 = tox_invite_friend(tox, friend, group_id);
	if (ret2 != 0) {
		error("ERROR: Couldn't invite friend\n");
	}
}

static void on_group_message(Tox *tox, int group, int friend, uint8_t *msg, uint16_t len,
		void *userdata) {
	void *ret;
	int ret2;
	const char *occ, *key = "ngb";

	(void)friend;
	(void)userdata;

	if (len < strlen(key) + 1) {
		return;
	}
	ret = memchr(msg, '%', len - strlen(key));
	if (ret) {
		occ = ret;
		if (memcmp(occ+1, key, strlen(key)) == 0) {
			info("INFO: Sending address\n");
			ret2 = tox_group_message_send(tox, group, ADDR, sizeof(ADDR));
			if (ret2 != 0) {
				error("ERROR: Failed to send group message\n");
			}
		}
	}
}

static void on_friend_request(Tox *tox, const uint8_t *public_key, const uint8_t *data,
		uint16_t length, void *userdata) {
	(void)data;
	(void)length;
	(void)userdata;

	info("INFO: Friend request\n");
	tox_add_friend_norequest(tox, public_key);
}

static bool load(Tox *tox) {
	int file;
	size_t nread = 0;
	ssize_t tmp;
	struct stat st;
	uint8_t *buf = NULL;

	file = open(GROUPFILE, O_RDONLY);
	if (file == -1) {
		info("WARN: Can't open %s\n", GROUPFILE);
		goto error;
	}
	if (fstat(file, &st) == -1) {
		info("WARN: Can't stat %s\n", GROUPFILE);
		return false;
	}
	buf = malloc((size_t)st.st_size);
	if (!buf) {
		info("WARN: OOM\n");
		return false;
	}
	while (nread < (size_t)st.st_size) {
		if ((tmp = read(file, buf+nread, (size_t)st.st_size - nread)) == -1) {
			info("WARN: Error reading from %s\n", GROUPFILE);
			goto error;
		}
		if (tmp == 0) {
			break;
		}
		nread += (size_t)tmp;
	}
	if (tox_load(tox, buf, (uint32_t)nread) == -1) {
		info("WARN: %s corrupeted\n", GROUPFILE);
		goto error;
	}

	free(buf);
	return true;
error:
	free(buf);
	return false;
}

static void save(Tox *tox) {
	size_t size, written = 0;
	ssize_t tmp;
	uint8_t *buf = NULL;
	int file;

	info("INFO: Saving\n");

	size = tox_size(tox);
	if (size == 0) {
		return;
	}
	buf = malloc(size);
	if (!buf) {
		info("WARN: OOM\n");
		return;
	}
	tox_save(tox, buf);
	file = open(GROUPFILE_TMP, O_WRONLY|O_CREAT, 0600);
	if (file == -1) {
		info("WARN: Can't open %s\n", GROUPFILE_TMP);
		goto exit;
	}
	while (written < size) {
		if ((tmp = write(file, buf+written, size-written)) == -1) {
			info("WARN: Error writing to file\n");
			goto exit;
		}
		written += (size_t)tmp;
	}
	close(file);
	if (rename(GROUPFILE_TMP, GROUPFILE) == -1) {
		info("WARN: Error renaming %s\n", GROUPFILE_TMP);
	}
exit:
	free(buf);
}

int main(void) {
	Tox *tox;
	uint8_t bs_key[TOX_CLIENT_ID_SIZE];
	uint8_t gb_addr[TOX_FRIEND_ADDRESS_SIZE];
	int ret;
	uint8_t addr[TOX_FRIEND_ADDRESS_SIZE];
	uint32_t interval;
	struct timespec req;
	char add_message[] = "Hello";
	time_t last_save = 0;

	parse_hex(bs_key, sizeof(bs_key), BS_KEY, strlen(BS_KEY));
	parse_hex(gb_addr, sizeof(gb_addr), GB_ADDR, strlen(GB_ADDR));

	tox = tox_new(1);
	if (!tox) {
		error("ERROR: Failed to create tox instance\n");
	}
	info("INFO: Tox instance created\n");

	if (!load(tox)) {
		tox_kill(tox);
		tox = tox_new(1);
		if (!tox) {
			error("ERROR: Failed to create tox instance\n");
		}

		gb_id = tox_add_friend(tox, gb_addr, (uint8_t *)add_message,
				(uint16_t)strlen(add_message));
		if (gb_id < 0) {
			error("ERROR: Adding Groupbot failed\n");
		}
		info("INFO: Groupbot added with id %jd\n", (intmax_t)gb_id);
	} else {
		info("INFO: Tox loaded\n");
	}

	ret = tox_bootstrap_from_address(tox, BS_IP, 1, htons(BS_PORT), bs_key);
	if (ret != 1) {
		error("ERROR: Bootstrapping failed\n");
	}
	info("INFO: Bootstrapping\n");

	tox_get_address(tox, addr);
	format_hex(ADDR, sizeof(ADDR), addr, sizeof(addr));
	printf("INFO: ");
	fwrite(ADDR, sizeof(ADDR), 1, stdout);
	puts("");

	ret = tox_set_name(tox, (uint8_t *)NICK, (uint16_t)strlen(NICK));
	if (ret != 0) {
		error("ERROR: Failed to set nick\n");
	}

	ret = tox_set_status_message(tox, (uint8_t *)STATUS, (uint16_t)strlen(STATUS));
	if (ret != 0) {
		error("ERROR: Failed to set status message\n");
	}

	tox_callback_status_message(tox, on_status_message, NULL);
	tox_callback_group_invite(tox, on_group_invite, NULL);
	tox_callback_friend_message(tox, on_friend_message, NULL);
	tox_callback_group_message(tox, on_group_message, NULL);
	tox_callback_friend_request(tox, on_friend_request, NULL);


	while (true) {
		tox_do(tox);

		if (send_invite) {
			char msg[] = "invite";
			uint32_t msg_id;

			msg_id = tox_send_message(tox, gb_id, (uint8_t *)msg,
					(uint16_t)strlen(msg));
			if (msg_id == 0) {
				error("ERROR: Failed to send \"invite\" to Groupbot\n");
			}
			send_invite = false;
		}

		if (last_save + 10 < time(NULL)) {
			save(tox);
			last_save = time(NULL);
		}

		interval = tox_do_interval(tox);
		req.tv_sec = 0;
		req.tv_nsec = interval * 1000 * 1000;
		nanosleep(&req, NULL);
	}
}
