/*
 * Copyright (C) 2015-2021 IoT.bzh Company
 * Author "Fulup Ar Foll"
 * Author: José Bollo <jose.bollo@iot.bzh>
 *
 * $RP_BEGIN_LICENSE$
 * Commercial License Usage
 *  Licensees holding valid commercial IoT.bzh licenses may use this file in
 *  accordance with the commercial license agreement provided with the
 *  Software or, alternatively, in accordance with the terms contained in
 *  a written agreement between you and The IoT.bzh Company. For licensing terms
 *  and conditions see https://www.iot.bzh/terms-conditions. For further
 *  information use the contact form at https://www.iot.bzh/contact.
 * 
 * GNU General Public License Usage
 *  Alternatively, this file may be used under the terms of the GNU General
 *  Public license version 3. This license is as published by the Free Software
 *  Foundation and appearing in the file LICENSE.GPLv3 included in the packaging
 *  of this file. Please review the following information to ensure the GNU
 *  General Public License requirements will be met
 *  https://www.gnu.org/licenses/gpl-3.0.html.
 * $RP_END_LICENSE$
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#if WITH_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include <systemd/sd-event.h>
#include <json-c/json.h>
#if !defined(JSON_C_TO_STRING_NOSLASHESCAPE)
#define JSON_C_TO_STRING_NOSLASHESCAPE 0
#endif

#include <libafbcli/afb-wsj1.h>
#include <libafbcli/afb-ws-client.h>
#include <libafbcli/afb-proto-ws.h>

enum {
	Exit_Success       = 0,
	Exit_Error         = 1,
	Exit_HangUp        = 2,
	Exit_Input_Fail    = 3,
	Exit_Bad_Arg       = 4,
	Exit_Cant_Connect  = 5,
	Exit_Line_Overflow = 6,
	Exit_Out_Of_Memory = 7
};

struct pending {
	char *line;
	struct pending *next;
};

/* declaration of functions */
static void on_wsj1_hangup(void *closure, struct afb_wsj1 *wsj1);
static void on_wsj1_call(void *closure, const char *api, const char *verb, struct afb_wsj1_msg *msg);
static void on_wsj1_event(void *closure, const char *event, struct afb_wsj1_msg *msg);

static void on_pws_hangup(void *closure);
static void on_pws_reply(void *closure, void *request, struct json_object *result, const char *error, const char *info);
static void on_pws_event_create(void *closure, uint16_t event_id, const char *event_name);
static void on_pws_event_remove(void *closure, uint16_t event_id);
static void on_pws_event_subscribe(void *closure, void *request, uint16_t event_id);
static void on_pws_event_unsubscribe(void *closure, void *request, uint16_t event_id);
static void on_pws_event_push(void *closure, uint16_t event_id, struct json_object *data);
static void on_pws_event_broadcast(void *closure, const char *event_name, struct json_object *data, const afb_proto_ws_uuid_t uuid, uint8_t hop);

static void emit_line(char *line);
static void process_line(char *line);
static int on_stdin(sd_event_source *src, int fd, uint32_t revents, void *closure);

static void wsj1_emit(const char *api, const char *verb, const char *object);
static void pws_call(const char *verb, const char *object);

/* the callback interface for wsj1 */
static struct afb_wsj1_itf wsj1_itf = {
	.on_hangup = on_wsj1_hangup,
	.on_call = on_wsj1_call,
	.on_event = on_wsj1_event
};

/* the callback interface for pws */
static struct afb_proto_ws_client_itf pws_itf = {
	.on_reply = on_pws_reply,
	.on_event_create = on_pws_event_create,
	.on_event_remove = on_pws_event_remove,
	.on_event_subscribe = on_pws_event_subscribe,
	.on_event_unsubscribe = on_pws_event_unsubscribe,
	.on_event_push = on_pws_event_push,
	.on_event_broadcast = on_pws_event_broadcast,
};

/* global variables */
static struct afb_wsj1 *wsj1;
static struct afb_proto_ws *pws;
static int breakcon;
static int callcount;
static int human;
static int raw;
static int keeprun;
static int direct;
static int echo;
static int ontty;
static int synchro;
static int usein;
static sd_event *loop;
static sd_event_source *evsrc;
static char *uuid;
static char *token;
static uint16_t numuuid;
static uint16_t numtoken;
static char *url;
static int exitcode = 0;
static struct pending *pendings_head = 0;
static struct pending *pendings_tail = 0;

/* print usage of the program */
static void usage(int status, char *arg0)
{
	char *name = strrchr(arg0, '/');
	name = name ? name + 1 : arg0;
	fprintf(status ? stderr : stdout, "usage: %s [options]... uri [api verb [data]]\n", name);
	fprintf(status ? stderr : stdout, "       %s -d [options]... uri [verb [data]]\n", name);
	fprintf(status ? stderr : stdout, "\n"
		"allowed options\n"
		"  -b, --break         Break connection just after event/call has been emitted.\n"
		"  -d, --direct        Direct api\n"
		"  -e, --echo          Echo inputs\n"
		"  -h, --help          Display this help\n"
		"  -H, --human         Display human readable JSON\n"
		"  -k, --keep-running  Keep running until disconnect, even if input closed\n"
		"  -p, --pipe COUNT    Allow to pipe COUNT requests\n"
		"  -r, --raw           Raw output (default)\n"
		"  -s, --sync          Synchronous: wait for answers (like -p 1)\n"
		"  -t, --token TOKEN   The token to use\n"
		"  -u, --uuid UUID     The identifier of session to use\n"
		"Example:\n"
		" %s --human 'localhost:1234/api' hello ping\n"
		"\n", name
	);

	exit(status);
}

/* entry function */
int main(int ac, char **av, char **env)
{
	int rc;
	char *a0, *an;

	/* get the program name */
	a0 = av[0];

	/* check options */
	while (ac > 1 && (an = av[1])[0] == '-') {
		if (an[1] == '-') {
			/* long option */

			if (!strcmp(an, "--human")) /* request for human output */
				human = 1;

			else if (!strcmp(an, "--raw")) /* request for raw output */
				raw = 1;

			else if (!strcmp(an, "--direct")) /* request for direct api */
				direct = 1;

			else if (!strcmp(an, "--break")) /* request to break connection */
				breakcon = 1;

			else if (!strcmp(an, "--keep-running")) /* request to break connection */
				keeprun = 1;

			else if (!strcmp(an, "--sync")) /* request to break connection */
				synchro = 1;

			else if (!strcmp(an, "--echo")) /* request to echo inputs */
				echo = 1;

			else if (!strcmp(an, "--pipe") && av[2] && atoi(av[2]) > 0) {
				synchro = atoi(av[2]);
				av++;
				ac--;
			}
			else if (!strcmp(an, "--token") && av[2]) { /* token to use */
				token = av[2];
				av++;
				ac--;
			}
			else if (!strcmp(an, "--uuid") && av[2]) { /* session id to join */
				uuid = av[2];
				av++;
				ac--;
			}
			/* emit usage and exit */
			else
				usage(strcmp(an, "--help") ? Exit_Bad_Arg : Exit_Success, a0);
		} else {
			/* short option(s) */
			for (rc = 1 ; an[rc] ; rc++)
				switch (an[rc]) {
				case 'H': human = 1; break;
				case 'r': raw = 1; break;
				case 'd': direct = 1; break;
				case 'b': breakcon = 1; break;
				case 'k': keeprun = 1; break;
				case 's': synchro = 1; break;
				case 'e': echo = 1; break;
				case 't': if (!av[2]) usage(Exit_Bad_Arg, a0); token = av[2]; av++; ac--; break;
				case 'u': if (!av[2]) usage(Exit_Bad_Arg, a0); uuid = av[2]; av++; ac--; break;
				case 'p': if (av[2] && atoi(av[2]) > 0) { synchro = atoi(av[2]); av++; ac--; break; } /*@fallthrough@*/
				default:
					usage(an[rc] != 'h' ? Exit_Bad_Arg : Exit_Success, a0);
					break;
				}
		}
		av++;
		ac--;
	}

	/* check the argument count */
	if (ac != 2 && ac != 4 && ac != 5)
		usage(1, a0);

	/* set raw by default */
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!human)
		raw = 1;

	/* get the default event loop */
	rc = sd_event_default(&loop);
	if (rc < 0) {
		fprintf(stderr, "connection to default event loop failed: %s\n", strerror(-rc));
		return 1;
	}

	/* connect the websocket wsj1 to the uri given by the first argument */
	if (direct) {
		pws = afb_ws_client_connect_api(loop, av[1], &pws_itf, NULL);
		if (pws == NULL) {
			fprintf(stderr, "connection to %s failed: %m\n", av[1]);
			return Exit_Cant_Connect;
		}
		afb_proto_ws_on_hangup(pws, on_pws_hangup);
		if (uuid) {
			numuuid = 1;
			afb_proto_ws_client_session_create(pws, numuuid, uuid);
		}
		if (token) {
			numtoken = 1;
			afb_proto_ws_client_token_create(pws, numtoken, token);
		}
	} else {
		rc = asprintf(&url, "%s%s%s%s%s%s%s",
			av[1],
			uuid || token ? "?" : "",
			uuid ? "uuid=" : "",
			uuid ?: "",
			uuid && token ? "&" : "",
			token ? "token=" : "",
			token ?: ""
		);
		wsj1 = afb_ws_client_connect_wsj1(loop, url, &wsj1_itf, NULL);
		if (wsj1 == NULL) {
			fprintf(stderr, "connection to %s failed: %m\n", av[1]);
			return Exit_Cant_Connect;
		}
	}

	/* test the behaviour */
	if (ac == 2) {
		/* get requests from stdin */
		usein = 1;
		ontty = isatty(0);
		fcntl(0, F_SETFL, O_NONBLOCK);
		if (sd_event_add_io(loop, &evsrc, 0, EPOLLIN, on_stdin, NULL) < 0)
			evsrc = NULL;
#if WITH_READLINE
		else if (ontty) {
			rl_callback_handler_install(0, process_line);
			atexit(rl_deprep_terminal);
		}
#endif
	} else {
		/* the request is defined by the arguments */
		usein = 0;
		if (direct)
			pws_call(av[2], av[3]);
		else
			wsj1_emit(av[2], av[3], av[4]);
	}

	/* loop until end */
	while (usein || keeprun || callcount) {
		sd_event_run(loop, 30000000);
	}
	return exitcode;
}

/* ensure allocation pointer succes */
static void ensure_allocation(void *p)
{
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(Exit_Out_Of_Memory);
	}
}

/* add a pending line */
static void pendings_add(char *line)
{
	struct pending *pending = malloc(sizeof *pending);
	ensure_allocation(pending);
	pending->line = line;
	pending->next = 0;
	*(!pendings_head ? &pendings_head : &pendings_tail->next) = pending;
	pendings_tail = pending;
}

/* get a pending line */
static char *pendings_get()
{
	struct pending *pending = pendings_head;
	char *result = pending->line;
	pendings_head = pending->next;
	free(pending);
	return result;
}

/* decrement the count of calls */
static void dec_callcount()
{
	char *line;

	callcount--;
	while (synchro && callcount < synchro && pendings_head) {
		line = pendings_get();
		emit_line(line);
		free(line);
	}

	if (synchro && callcount < synchro && evsrc)
		sd_event_source_set_io_events(evsrc, EPOLLIN);
}

/* increment the count of calls */
static void inc_callcount()
{
	callcount++;
	if (synchro && callcount >= synchro && evsrc)
		sd_event_source_set_io_events(evsrc, 0);
}

/* called when wsj1 hangsup */
static void on_wsj1_hangup(void *closure, struct afb_wsj1 *wsj1)
{
	printf("ON-HANGUP\n");
	fflush(stdout);
	exit(Exit_HangUp);
}

/* called when wsj1 receives a method invocation */
static void on_wsj1_call(void *closure, const char *api, const char *verb, struct afb_wsj1_msg *msg)
{
	int rc;
	if (raw)
		printf("%s\n", afb_wsj1_msg_object_s(msg, 0));
	if (human)
		printf("ON-CALL %s/%s:\n%s\n", api, verb,
				json_object_to_json_string_ext(afb_wsj1_msg_object_j(msg),
							JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
	fflush(stdout);
	rc = afb_wsj1_reply_error_s(msg, "\"unimplemented\"", NULL);
	if (rc < 0)
		fprintf(stderr, "replying failed: %m\n");
}

/* called when wsj1 receives an event */
static void on_wsj1_event(void *closure, const char *event, struct afb_wsj1_msg *msg)
{
	if (raw)
		printf("%s\n", afb_wsj1_msg_object_s(msg, 0));
	if (human)
		printf("ON-EVENT %s:\n%s\n", event,
				json_object_to_json_string_ext(afb_wsj1_msg_object_j(msg),
							JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
	fflush(stdout);
}

/* called when wsj1 receives a reply */
static void on_wsj1_reply(void *closure, struct afb_wsj1_msg *msg)
{
	int iserror = !afb_wsj1_msg_is_reply_ok(msg);
	exitcode = iserror ? Exit_Error : Exit_Success;
	if (raw)
		printf("%s\n", afb_wsj1_msg_object_s(msg, 0));
	if (human)
		printf("ON-REPLY %s: %s\n%s\n", (char*)closure,
				iserror ? "ERROR" : "OK",
				json_object_to_json_string_ext(afb_wsj1_msg_object_j(msg),
							JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
	fflush(stdout);
	free(closure);
	dec_callcount();
}

/* makes a call */
static void wsj1_call(const char *api, const char *verb, const char *object)
{
	static int num = 0;
	char *key;
	int rc;

	/* allocates an id for the request */
	rc = asprintf(&key, "%d:%s/%s", ++num, api, verb);

	/* echo the command if asked */
	if (echo)
		printf("SEND-CALL %s/%s %s\n", api, verb, object?:"null");

	/* send the request */
	inc_callcount();
	rc = afb_wsj1_call_s(wsj1, api, verb, object, on_wsj1_reply, key);
	if (rc < 0) {
		fprintf(stderr, "calling %s/%s(%s) failed: %m\n", api, verb, object);
		dec_callcount();
	}
}

/* sends an event */
static void wsj1_event(const char *event, const char *object)
{
	int rc;

	/* echo the command if asked */
	if (echo)
		printf("SEND-EVENT: %s %s\n", event, object?:"null");

	rc = afb_wsj1_send_event_s(wsj1, event, object);
	if (rc < 0)
		fprintf(stderr, "sending !%s(%s) failed: %m\n", event, object);
}

/* emits either a call (when api!='!') or an event */
static void wsj1_emit(const char *api, const char *verb, const char *object)
{
	if (object == NULL || object[0] == 0)
		object = "null";

	if (api[0] == '!' && api[1] == 0)
		wsj1_event(verb, object);
	else
		wsj1_call(api, verb, object);
}

static char sep[] = " \t";

/* emit call for the line */
static void emit_line(char *line)
{
	char *f1, *f2, *rem;

	/* normalise the buffer content */
	/* TODO: handle backspace \x7f ? */
	/* process the lines */
	f1 = &line[strspn(line, sep)];
	f2 = &f1[strcspn(f1, sep)];
	if (*f2)
		*f2++ = 0;
	f2 = &f2[strspn(f2, sep)];
	rem = &f2[strcspn(f2, sep)];
	if (*rem)
		*rem++ = 0;
	rem = &rem[strspn(rem, sep)];

	if (direct)
		pws_call(f1, rem);
	else if (f2[0])
		wsj1_emit(f1, f2, rem);
	else
		fprintf(stderr, "verb missing, bad line: %s\n", line);

	if (breakcon)
		exit(0);
}

/* process stdin */
static void process_line(char *line)
{
	char *head;

	if (!line) {
		usein = 0;
		return;
	}

#if WITH_READLINE
	if (*line)
		add_history(line);
#endif

	head = &line[strspn(line, sep)];
	if (*head && *head != '#') {
		if (synchro && callcount >= synchro) {
			pendings_add(line);
			return;
		}
		emit_line(line);
	}
	free(line);
}

/* process stdin */
static void process_stdin()
{
	static size_t pos = 0;
	static size_t count = 0;
	static char line[16384];

	ssize_t rc = 0;
	char *l;

	/* read the buffer */
	while (sizeof line > count) {
		rc = read(0, line + count, sizeof line - count);
		if (rc >= 0 || errno != EINTR)
			break;
	}
	if (rc < 0) {
		if (errno != EAGAIN) {
			fprintf(stderr, "read error: %m\n");
			exit(Exit_Input_Fail);
		}
	}
	else {
		count += (size_t)rc;
	}
	while (pos < count) {
		if (line[pos] != '\n') {
			pos++;
			if (pos >= sizeof line) {
				fprintf(stderr, "line overflow\n");
				exit(Exit_Line_Overflow);
			}
		}
		else {
			l = strndup(line, pos);
			ensure_allocation(l);
			if (++pos < count)
				memmove(line, line + pos, count - pos);
			count -= pos;
			pos = 0;
			process_line(l);
		}
	}
	if (rc == 0 && count == 0) {
		process_line(0);
	}
}

/* called when something happens on stdin */
static int on_stdin(sd_event_source *src, int fd, uint32_t revents, void *closure)
{
#if WITH_READLINE
	if (ontty)
		rl_callback_read_char();
	else
#endif
		process_stdin();
	if (!usein) {
#if WITH_READLINE
		rl_callback_handler_remove();
#endif
		sd_event_source_unref(src);
		evsrc = NULL;
	}
	return 1;
}

static void on_pws_reply(void *closure, void *request, struct json_object *result, const char *error, const char *info)
{
	int iserror = !!error;
	exitcode = iserror ? Exit_Error : Exit_Success;
	error = error ?: "success";
	if (raw) {
		/* TODO: transitionnal: fake the structured response */
		struct json_object *x = json_object_new_object(), *y = json_object_new_object();
		json_object_object_add(x, "jtype", json_object_new_string("afb-reply"));
		json_object_object_add(x, "request", y);
		json_object_object_add(y, "status", json_object_new_string(error));
		if (info)
			json_object_object_add(y, "info", json_object_new_string(info));
		if (result)
			json_object_object_add(x, "response", json_object_get(result));

		printf("%s\n", json_object_to_json_string_ext(x, JSON_C_TO_STRING_NOSLASHESCAPE));
		json_object_put(x);
	}
	if (human)
		printf("ON-REPLY %s: %s %s\n%s\n", (char*)request, error, info ?: "", json_object_to_json_string_ext(result, JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
	fflush(stdout);
	free(request);
	dec_callcount();
}

static void on_pws_event_create(void *closure, uint16_t event_id, const char *event_name)
{
	printf("ON-EVENT-CREATE: [%d:%s]\n", event_id, event_name);
	fflush(stdout);
}

static void on_pws_event_remove(void *closure, uint16_t event_id)
{
	printf("ON-EVENT-REMOVE: [%d]\n", event_id);
	fflush(stdout);
}

static void on_pws_event_subscribe(void *closure, void *request, uint16_t event_id)
{
	printf("ON-EVENT-SUBSCRIBE %s: [%d]\n", (char*)request, event_id);
	fflush(stdout);
}

static void on_pws_event_unsubscribe(void *closure, void *request, uint16_t event_id)
{
	printf("ON-EVENT-UNSUBSCRIBE %s: [%d]\n", (char*)request, event_id);
	fflush(stdout);
}

static void on_pws_event_push(void *closure, uint16_t event_id, struct json_object *data)
{
	if (raw)
		printf("ON-EVENT-PUSH: [%d]\n%s\n", event_id, json_object_to_json_string_ext(data, JSON_C_TO_STRING_NOSLASHESCAPE));
	if (human)
		printf("ON-EVENT-PUSH: [%d]\n%s\n", event_id, json_object_to_json_string_ext(data, JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
	fflush(stdout);
}

static void on_pws_event_broadcast(void *closure, const char *event_name, struct json_object *data, const afb_proto_ws_uuid_t uuid, uint8_t hop)
{
	if (raw)
		printf("ON-EVENT-BROADCAST: [%s]\n%s\n", event_name, json_object_to_json_string_ext(data, JSON_C_TO_STRING_NOSLASHESCAPE));
	if (human)
		printf("ON-EVENT-BROADCAST: [%s]\n%s\n", event_name, json_object_to_json_string_ext(data, JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
	fflush(stdout);
}

/* makes a call */
static void pws_call(const char *verb, const char *object)
{
	static int num = 0;
	char *key;
	int rc;
	struct json_object *o;
	enum json_tokener_error jerr;

	/* allocates an id for the request */
	rc = asprintf(&key, "%d:%s", ++num, verb);

	/* echo the command if asked */
	if (echo)
		printf("SEND-CALL: %s %s\n", verb, object?:"null");

	/* send the request */
	inc_callcount();
	if (object == NULL || object[0] == 0)
		o = NULL;
	else {
		o = json_tokener_parse_verbose(object, &jerr);
		if (jerr != json_tokener_success)
			o = json_object_new_string(object);
	}
	rc = afb_proto_ws_client_call(pws, verb, o, numuuid, numtoken, key, NULL);
	json_object_put(o);
	if (rc < 0) {
		fprintf(stderr, "calling %s(%s) failed: %m\n", verb, object?:"");
		dec_callcount();
	}
}

/* called when pws hangsup */
static void on_pws_hangup(void *closure)
{
	printf("ON-HANGUP\n");
	fflush(stdout);
	exit(Exit_HangUp);
}
