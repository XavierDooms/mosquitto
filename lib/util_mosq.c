/*
Copyright (c) 2009-2018 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#include <assert.h>
#include <string.h>

#ifdef WIN32
#  include <winsock2.h>
#  include <aclapi.h>
#  include <io.h>
#  include <lmcons.h>
#else
#  include <sys/stat.h>
#endif


#ifdef WITH_BROKER
#include "mosquitto_broker_internal.h"
#endif

#include "mosquitto.h"
#include "memory_mosq.h"
#include "net_mosq.h"
#include "send_mosq.h"
#include "time_mosq.h"
#include "tls_mosq.h"
#include "util_mosq.h"

#ifdef WITH_WEBSOCKETS
#include <libwebsockets.h>
#endif

#ifdef WITH_BROKER
void mosquitto__check_keepalive(struct mosquitto_db *db, struct mosquitto *mosq)
#else
void mosquitto__check_keepalive(struct mosquitto *mosq)
#endif
{
	time_t next_msg_out;
	time_t last_msg_in;
	time_t now = mosquitto_time();
#ifndef WITH_BROKER
	int rc;
#endif

	assert(mosq);
#if defined(WITH_BROKER) && defined(WITH_BRIDGE)
	/* Check if a lazy bridge should be timed out due to idle. */
	if(mosq->bridge && mosq->bridge->start_type == bst_lazy
				&& mosq->sock != INVALID_SOCKET
				&& now - mosq->next_msg_out - mosq->keepalive >= mosq->bridge->idle_timeout){

		log__printf(NULL, MOSQ_LOG_NOTICE, "Bridge connection %s has exceeded idle timeout, disconnecting.", mosq->id);
		net__socket_close(db, mosq);
		return;
	}
#endif
	pthread_mutex_lock(&mosq->msgtime_mutex);
	next_msg_out = mosq->next_msg_out;
	last_msg_in = mosq->last_msg_in;
	pthread_mutex_unlock(&mosq->msgtime_mutex);
	if(mosq->keepalive && mosq->sock != INVALID_SOCKET &&
			(now >= next_msg_out || now - last_msg_in >= mosq->keepalive)){

		if(mosq->state == mosq_cs_connected && mosq->ping_t == 0){
			send__pingreq(mosq);
			/* Reset last msg times to give the server time to send a pingresp */
			pthread_mutex_lock(&mosq->msgtime_mutex);
			mosq->last_msg_in = now;
			mosq->next_msg_out = now + mosq->keepalive;
			pthread_mutex_unlock(&mosq->msgtime_mutex);
		}else{
#ifdef WITH_BROKER
			if(mosq->listener){
				mosq->listener->client_count--;
				assert(mosq->listener->client_count >= 0);
			}
			mosq->listener = NULL;
			net__socket_close(db, mosq);
#else
			net__socket_close(mosq);
			pthread_mutex_lock(&mosq->state_mutex);
			if(mosq->state == mosq_cs_disconnecting){
				rc = MOSQ_ERR_SUCCESS;
			}else{
				rc = MOSQ_ERR_KEEPALIVE;
			}
			pthread_mutex_unlock(&mosq->state_mutex);
			pthread_mutex_lock(&mosq->callback_mutex);
			if(mosq->on_disconnect){
				mosq->in_callback = true;
				mosq->on_disconnect(mosq, mosq->userdata, rc);
				mosq->in_callback = false;
			}
			if(mosq->on_disconnect_v5){
				mosq->in_callback = true;
				mosq->on_disconnect_v5(mosq, mosq->userdata, rc, NULL);
				mosq->in_callback = false;
			}
			pthread_mutex_unlock(&mosq->callback_mutex);
#endif
		}
	}
}

uint16_t mosquitto__mid_generate(struct mosquitto *mosq)
{
	/* FIXME - this would be better with atomic increment, but this is safer
	 * for now for a bug fix release.
	 *
	 * If this is changed to use atomic increment, callers of this function
	 * will have to be aware that they may receive a 0 result, which may not be
	 * used as a mid.
	 */
	uint16_t mid;
	assert(mosq);

	pthread_mutex_lock(&mosq->mid_mutex);
	mosq->last_mid++;
	if(mosq->last_mid == 0) mosq->last_mid++;
	mid = mosq->last_mid;
	pthread_mutex_unlock(&mosq->mid_mutex);

	return mid;
}


#ifdef WITH_TLS_PSK
int mosquitto__hex2bin(const char *hex, unsigned char *bin, int bin_max_len)
{
	BIGNUM *bn = NULL;
	int len;
	int leading_zero = 0;
	int start = 0;
	int i = 0;

	/* Count the number of leading zero */
	for(i=0; i<strlen(hex); i=i+2) {
		if(strncmp(hex + i, "00", 2) == 0) {
			leading_zero++;
			/* output leading zero to bin */
			bin[start++] = 0;
		}else{
			break;
		}
	}

	if(BN_hex2bn(&bn, hex) == 0){
		if(bn) BN_free(bn);
		return 0;
	}
	if(BN_num_bytes(bn) + leading_zero > bin_max_len){
		BN_free(bn);
		return 0;
	}

	len = BN_bn2bin(bn, bin + leading_zero);
	BN_free(bn);
	return len + leading_zero;
}
#endif

FILE *mosquitto__fopen(const char *path, const char *mode, bool restrict_read)
{
#ifdef WIN32
	char buf[4096];
	int rc;
	rc = ExpandEnvironmentStrings(path, buf, 4096);
	if(rc == 0 || rc > 4096){
		return NULL;
	}else{
		if (restrict_read) {
			HANDLE hfile;
			SECURITY_ATTRIBUTES sec;
			EXPLICIT_ACCESS ea;
			PACL pacl = NULL;
			char username[UNLEN + 1];
			int ulen = UNLEN;
			SECURITY_DESCRIPTOR sd;

			GetUserName(username, &ulen);
			if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
				return NULL;
			}
			BuildExplicitAccessWithName(&ea, username, GENERIC_ALL, SET_ACCESS, NO_INHERITANCE);
			if (SetEntriesInAcl(1, &ea, NULL, &pacl) != ERROR_SUCCESS) {
				return NULL;
			}
			if (!SetSecurityDescriptorDacl(&sd, TRUE, pacl, FALSE)) {
				LocalFree(pacl);
				return NULL;
			}

			sec.nLength = sizeof(SECURITY_ATTRIBUTES);
			sec.bInheritHandle = FALSE;
			sec.lpSecurityDescriptor = &sd;

			hfile = CreateFile(buf, GENERIC_READ | GENERIC_WRITE, 0,
				&sec,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL,
				NULL);

			LocalFree(pacl);

			int fd = _open_osfhandle((intptr_t)hfile, 0);
			if (fd < 0) {
				return NULL;
			}

			FILE *fptr = _fdopen(fd, mode);
			if (!fptr) {
				_close(fd);
				return NULL;
			}
			return fptr;

		}else {
			return fopen(buf, mode);
		}
	}
#else
	if (restrict_read) {
		FILE *fptr;
		mode_t old_mask;

		old_mask = umask(0077);
		fptr = fopen(path, mode);
		umask(old_mask);

		return fptr;
	}else{
		return fopen(path, mode);
	}
#endif
}
