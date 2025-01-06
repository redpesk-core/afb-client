/*
 * Copyright (C) 2015-2025 IoT.bzh Company
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
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdarg.h>

#if WITH_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#define HISTORY_FILE  ".config/afb-client.history"
static void rlhexitcb();
static char history_file_path[PATH_MAX];
#endif

#include <systemd/sd-event.h>
#include <json-c/json.h>
#if !defined(JSON_C_TO_STRING_NOSLASHESCAPE)
#define JSON_C_TO_STRING_NOSLASHESCAPE 0
#endif

#ifndef WS_MAXLEN_MIN
# define WS_MAXLEN_MIN 16384
#endif

/*!!! HACK SINCE libafb 5.2.1 the 2 below declarations must be set !!!*/
/*!!! HACK this is a temporary fix                                 !!!*/
#define WITH_WSAPI 1
#define WITH_WSJ1  1

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
	Exit_Out_Of_Memory = 7,
	Exit_Fatal_Internal= 8
};

struct pending {
	char *line;
	struct pending *next;
};

struct buffer {
	char *value;
	size_t length;
	size_t offset;
	struct buffer *next;
	int file;
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

static int onout(sd_event_source *src, int fd, uint32_t revents, void *closure);
static int print(const char *fmt, ...);
static int error(const char *fmt, ...);

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
static int raw = 1;
static int quiet;
static int keeprun;
static int direct;
static int echo;
static int ontty;
static int synchro;
static int usein;
static sd_event *loop;
static sd_event_source *evsrc;
static char *uuid;
static char *wsmaxlen;
static size_t ws_max_length;
static char *token;
static uint16_t numuuid;
static uint16_t numtoken;
static char *url;
static int exitcode = 0;
static struct pending *pendings_head = 0;
static struct pending *pendings_tail = 0;
static struct buffer *buffers_head = 0;
static struct buffer *buffers_tail = 0;

/* print usage of the program */
static void usage(int status, char *arg0)
{
	int (*prt)(const char *fmt, ...) = status ? error : print;
	char *name = strrchr(arg0, '/');
	name = name ? name + 1 : arg0;
	prt("usage: %s [options]... uri [api verb data]\n", name);
	prt("       %s -d [options]... uri [verb data]\n", name);
	prt("\n"
		"allowed options\n"
		"  -b, --break         Break connection just after event/call has been emitted.\n"
		"  -d, --direct        Direct api\n"
		"  -e, --echo          Echo inputs\n"
		"  -h, --help          Display this help\n"
		"  -H, --human         Display human readable JSON\n"
		"  -k, --keep-running  Keep running until disconnect, even if input closed\n"
		"  -p, --pipe COUNT    Allow to pipe COUNT requests\n"
		"  -q, --quiet         Less output\n"
		"  -r, --raw           Raw output (default)\n"
		"  -s, --sync          Synchronous: wait for answers (like -p 1)\n"
		"  -t, --token TOKEN   The token to use\n"
		"  -u, --uuid UUID     The identifier of session to use\n"
		"  -v, --version       Print the version and exits\n"
		"  -w, --ws-maxlen VAL Set maximum length of websocket messages\n"
		"\n"
		"Data must be the last argument (use quoting on need).\n"
		"Data can be - (a single dash), in that case data is read from stdin.\n"
		"\n"
		"Example:\n"
	);
	prt(	" %s -H localhost:1234/api hello ping '{\"key1\":[1,2,3,true],\"key2\":\"item\"}'\n", name);
	prt(	" %s -H localhost:1234/api hello ping null\n", name);
	prt(	" echo '[1,false,\"item\",null]' | %s -H localhost:1234/api hello ping -\n", name);
	prt("\n");

	exit(status);
}

static void version(char *arg0)
{
	char *name = strrchr(arg0, '/');
	name = name ? name + 1 : arg0;

	printf("\n%s %s\nCopyright (C) 2015-2025 IoT.bzh Company\n\n", name, VERSION);
	exit(0);
}

static char *readfile(FILE *file)
{
	char buffer[4096];
	size_t szr, size = 0;
	char *nres, *result = NULL;

	for(;;) {
		clearerr(file);
		szr = fread(buffer, 1, sizeof buffer, file);
		if (ferror(file)) {
			error("error while reading file\n");
			exit(1);
		}
		if (szr > 0) {
			nres = realloc(result, size + szr + 1);
			if (nres == NULL) {
				error("out of memory\n");
				exit(1);
			}
			result = nres;
			memcpy(&result[size], buffer, szr);
			size += szr;
			result[size] = 0;
		}
		if (feof(file))
			return result == NULL ? "null" : result;
	}
}

static const char *cmdarg(char *cmd)
{
	if (cmd == NULL) {
		error("implicit null in arguments is deprecated and will be removed soon.\n");
		return "null";
	}

	/* check if 'cmd' equals "-" */
	if (cmd[0] != '-' || cmd[1] != 0)
		return cmd; /* no, then returns it */

	/* read from stdin */
	return readfile(stdin);
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
				raw = 0;

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
			else if (!strcmp(an, "--quiet")) /* request less output */
				quiet = 1;

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
			else if (!strcmp(an, "--version")) { /* print version */
				version(a0);
			}
			else if (!strcmp(an, "--ws-maxlen") && av[2]) { /* maximum websocket length */
				wsmaxlen = av[2];
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
				case 'H': raw = 0; break;
				case 'r': raw = 1; break;
				case 'd': direct = 1; break;
				case 'b': breakcon = 1; break;
				case 'k': keeprun = 1; break;
				case 's': synchro = 1; break;
				case 'e': echo = 1; break;
				case 't': if (!av[2]) usage(Exit_Bad_Arg, a0); token = av[2]; av++; ac--; break;
				case 'u': if (!av[2]) usage(Exit_Bad_Arg, a0); uuid = av[2]; av++; ac--; break;
				case 'p': if (av[2] && atoi(av[2]) > 0) { synchro = atoi(av[2]); av++; ac--; break; } /*@fallthrough@*/
				case 'q': quiet = 1; break;
				case 'v': version(a0); break;
				case 'w': if (!av[2]) usage(Exit_Bad_Arg, a0); wsmaxlen = av[2]; av++; ac--; break;
				default:
					usage(an[rc] != 'h' ? Exit_Bad_Arg : Exit_Success, a0);
					break;
				}
		}
		av++;
		ac--;
	}

	/* get maxlen */
	if (wsmaxlen) {
		long wml = strtol(wsmaxlen, &wsmaxlen, 10);
		if (wsmaxlen[0] != 0 || wml < 0) {
			error("bad value for option --ws-maxlen\n");
			return 1;
		}
		if (wml < WS_MAXLEN_MIN) {
			error("value of --ws-maxlen is too small (mini is %d)\n", WS_MAXLEN_MIN);
			return 1;
		}
		ws_max_length = (size_t)wml;
	}

	/* check the argument count here ac is 2 + count */
	if (ac == 1) {
		error("missing uri\n");
		return 1;
	}
	else if (ac == 2)
		;/* do nothing, it is okay */
	else if (direct && (ac != 3 && ac != 4)) {
		error("extra arguments (clue: check quoting of last argument)\n");
		return 1;
	}
	else if (!direct && (ac != 4 && ac != 5)) {
		if (ac < 4)
			error("missing verb\n");
		else
			error("extra arguments (clue: check quoting of last argument)\n");
		return 1;
	}

	/* set raw by default */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* get the default event loop */
	rc = sd_event_default(&loop);
	if (rc < 0) {
		error("connection to default event loop failed: %s\n", strerror(-rc));
		return 1;
	}

	/* connect the websocket wsj1 to the uri given by the first argument */
	if (direct) {
		pws = afb_ws_client_connect_api(loop, av[1], &pws_itf, NULL);
		if (pws == NULL) {
			error("connection to %s failed: %m\n", av[1]);
			return Exit_Cant_Connect;
		}
		if (wsmaxlen)
			afb_proto_ws_set_max_length(pws, ws_max_length);
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
			error("connection to %s failed: %m\n", av[1]);
			return Exit_Cant_Connect;
		}
		if (wsmaxlen)
			afb_wsj1_set_max_length(wsj1, ws_max_length);
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
			snprintf(history_file_path, sizeof history_file_path, "%s/%s",
							getenv("HOME")?:"", HISTORY_FILE);
			read_history(history_file_path);
			history_set_pos(history_length);
			atexit(rlhexitcb);
		}
#endif
	} else {
		/* the request is defined by the arguments */
		usein = 0;
		if (direct)
			pws_call(av[2], cmdarg(av[3]));
		else
			wsj1_emit(av[2], av[3], cmdarg(av[4]));
	}

	/* loop until end */
	while (usein || keeprun || callcount) {
		sd_event_run(loop, 30000000);
	}
	return exitcode;
}

#if WITH_READLINE
static void rlhexitcb()
{
	rl_deprep_terminal();
	write_history(history_file_path);
}
#endif

/* exit when out of memory */
static void leave(int code, const char *msg)
{
	if (msg)
		write(2, msg, strlen(msg));
	exit(code);
}


/* exit when out of memory */
static void oom()
{
	leave(Exit_Out_Of_Memory, "\nout of memory - emergency exit\n");
}

static void fatal()
{
	leave(Exit_Fatal_Internal, "\ninternal fatal error - emergency exit\n");
}

/* ensure allocation pointer succes */
static void ensure_allocation(void *p)
{
	if (!p)
		oom();
}

/* get a buffer line */
static void flush_buffers()
{
	ssize_t rc;
	struct buffer *buffer;
	sd_event_source *src;

	while((buffer = buffers_head)) {
		rc = write(buffer->file, &buffer->value[buffer->offset], buffer->length - buffer->offset);
		if (rc < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (sd_event_add_io(loop, &src, buffer->file, EPOLLOUT, onout, NULL) < 0)
					fatal();
				break;
			}
			if (errno != EINTR) {
				fatal();
			}
		}
		else {
			buffer->offset += (size_t)rc;
			if (buffer->offset == buffer->length) {
				buffers_head = buffer->next;
				free(buffer->value);
				free(buffer);
			}
		}
	}
}

static int onout(sd_event_source *src, int fd, uint32_t revents, void *closure)
{
	sd_event_source_unref(src);
	flush_buffers();
	return 0;
}

static int out(int file, const char *fmt, va_list ap)
{
	int rc;
	struct buffer *buffer = malloc(sizeof *buffer);
	buffer->file = file;
	rc = vasprintf(&buffer->value, fmt, ap);
	if (rc < 0) { oom(); return rc; }
	if (rc == 0) {
		free(buffer);
	}
	else {
		buffer->length = (unsigned)rc;
		buffer->offset = 0;
		buffer->next = 0;
		*(!buffers_head ? &buffers_head : &buffers_tail->next) = buffer;
		buffers_tail = buffer;
	}
	flush_buffers();
	return 0;
}

static int print(const char *fmt, ...)
{
	int r;
	va_list ap;
	va_start(ap, fmt);
	r = out(1, fmt, ap);
	va_end(ap);
	return r;
}

static int error(const char *fmt, ...)
{
	int r;
	va_list ap;
	va_start(ap, fmt);
	r = out(2, fmt, ap);
	va_end(ap);
	return r;
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
	if (!quiet)
		print("ON-HANGUP\n");
	exit(Exit_HangUp);
}

/* called when wsj1 receives a method invocation */
static void on_wsj1_call(void *closure, const char *api, const char *verb, struct afb_wsj1_msg *msg)
{
	int rc;
	if (!quiet)
		print("ON-CALL %s/%s:\n", api, verb);
	if (raw)
		print("%s\n", afb_wsj1_msg_object_s(msg, 0));
	else
		print("%s\n", json_object_to_json_string_ext(afb_wsj1_msg_object_j(msg),
							JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
	rc = afb_wsj1_reply_error_s(msg, "\"unimplemented\"", NULL);
	if (rc < 0)
		error("replying failed: %m\n");
}

/* called when wsj1 receives an event */
static void on_wsj1_event(void *closure, const char *event, struct afb_wsj1_msg *msg)
{
	if (!quiet)
		print("ON-EVENT %s:\n", event);
	if (raw)
		print("%s\n", afb_wsj1_msg_object_s(msg, 0));
	else
		print("%s\n", json_object_to_json_string_ext(afb_wsj1_msg_object_j(msg),
							JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
}

/* called when wsj1 receives a reply */
static void on_wsj1_reply(void *closure, struct afb_wsj1_msg *msg)
{
	int iserror = !afb_wsj1_msg_is_reply_ok(msg);
	exitcode = iserror ? Exit_Error : Exit_Success;
	if (!quiet)
		print("ON-REPLY %s: %s\n", (char*)closure, iserror ? "ERROR" : "OK");
	if (raw)
		print("%s\n", afb_wsj1_msg_object_s(msg, 0));
	else
		print("%s\n", json_object_to_json_string_ext(afb_wsj1_msg_object_j(msg),
							JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
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
		print("SEND-CALL %s/%s %s\n", api, verb, object?:"null");

	/* send the request */
	inc_callcount();
	rc = afb_wsj1_call_s(wsj1, api, verb, object, on_wsj1_reply, key);
	if (rc < 0) {
		error("calling %s/%s(%s) failed: %m\n", api, verb, object);
		dec_callcount();
	}
}

/* sends an event */
static void wsj1_event(const char *event, const char *object)
{
	int rc;

	/* echo the command if asked */
	if (echo)
		print("SEND-EVENT: %s %s\n", event, object?:"null");

	rc = afb_wsj1_send_event_s(wsj1, event, object);
	if (rc < 0)
		error("sending !%s(%s) failed: %m\n", event, object);
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
	size_t x;
	char *f1, *f2, *rem;

	/* normalise the buffer content */
	/* TODO: handle backspace \x7f ? */
	/* process the lines */
	f1 = &line[strspn(line, sep)];
	x = strcspn(f1, sep);

	/* check if system exec requested */
	if (f1[0] == '!' && x > 1) {
		system(&f1[1]);
		return;
	}
	f2 = &f1[x];
	if (*f2)
		*f2++ = 0;
	f2 = &f2[strspn(f2, sep)];

	if (direct)
		pws_call(f1, f2);
	else if (f2[0]) {
		rem = &f2[strcspn(f2, sep)];
		if (*rem)
			*rem++ = 0;
		rem = &rem[strspn(rem, sep)];
		wsj1_emit(f1, f2, rem);
	}
	else
		error("verb missing, bad line: %s\n", line);

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
	static char   line[16384];
	static char  *prvline = NULL;
	static size_t prvsize = 0;

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
			error("read error: %m\n");
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
				l = realloc(prvline, prvsize + pos);
				ensure_allocation(l);
				memcpy(&l[prvsize], line, pos);
				prvsize += pos;
				prvline = l;
				count -= pos;
				pos = 0;
			}
		}
		else if (pos > 0) {
			l = realloc(prvline, prvsize + pos + 1);
			ensure_allocation(l);
			memcpy(&l[prvsize], line, pos);
			l[prvsize + pos] = 0;
			if (++pos < count)
				memmove(line, line + pos, count - pos);
			count -= pos;
			pos = 0;
			prvsize = 0;
			prvline = NULL;
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
	if (!quiet)
		print("ON-REPLY %s: %s %s\n", (char*)request, error, info ?: "");
	if (raw)
		print("%s\n", json_object_to_json_string_ext(result, JSON_C_TO_STRING_NOSLASHESCAPE));
	else
		print("%s\n", json_object_to_json_string_ext(result, JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
	free(request);
	dec_callcount();
}

static void on_pws_event_create(void *closure, uint16_t event_id, const char *event_name)
{
	if (!quiet)
		print("ON-EVENT-CREATE: [%d:%s]\n", event_id, event_name);
}

static void on_pws_event_remove(void *closure, uint16_t event_id)
{
	if (!quiet)
		print("ON-EVENT-REMOVE: [%d]\n", event_id);
}

static void on_pws_event_subscribe(void *closure, void *request, uint16_t event_id)
{
	if (!quiet)
		print("ON-EVENT-SUBSCRIBE %s: [%d]\n", (char*)request, event_id);
}

static void on_pws_event_unsubscribe(void *closure, void *request, uint16_t event_id)
{
	if (!quiet)
		print("ON-EVENT-UNSUBSCRIBE %s: [%d]\n", (char*)request, event_id);
}

static void on_pws_event_push(void *closure, uint16_t event_id, struct json_object *data)
{
	if (!quiet)
		print("ON-EVENT-PUSH: [%d]\n", event_id);
	if (raw)
		print("%s\n", json_object_to_json_string_ext(data, JSON_C_TO_STRING_NOSLASHESCAPE));
	else
		print("%s\n", json_object_to_json_string_ext(data, JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
}

static void on_pws_event_broadcast(void *closure, const char *event_name, struct json_object *data, const afb_proto_ws_uuid_t uuid, uint8_t hop)
{
	if (!quiet)
		print("ON-EVENT-BROADCAST: [%s]\n", event_name);
	if (raw)
		print("%s\n", json_object_to_json_string_ext(data, JSON_C_TO_STRING_NOSLASHESCAPE));
	else
		print("%s\n", json_object_to_json_string_ext(data, JSON_C_TO_STRING_PRETTY|JSON_C_TO_STRING_NOSLASHESCAPE));
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
		print("SEND-CALL: %s %s\n", verb, object?:"null");

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
		error("calling %s(%s) failed: %m\n", verb, object?:"");
		dec_callcount();
	}
}

/* called when pws hangsup */
static void on_pws_hangup(void *closure)
{
	if (!quiet)
		print("ON-HANGUP\n");
	exit(Exit_HangUp);
}
