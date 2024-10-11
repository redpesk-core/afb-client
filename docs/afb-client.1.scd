afb-client(1)	"Redpesk OS"

# NAME

afb-client - Simple client for connecting with *afb-binder*.

# SYNOPSIS

*afb-client* [<_options_>...] _SOCKSPEC_ [api verb data]
*afb-client* [<_options_>...] -d _SOCKSPEC_ [verb data]


# DESCRIPTION

The program *afb-client* is usefull for communicating with
APIs offered by an instance *afb-binder* process.
The current version is able to communicate on WS/HTTP, WSS/HTTPS
or WSAPI interfaces exposed by *afb-binder*.

## Connection to WS/HTTP interface

Without the option *-d* (or *--direct*), *afb-client* connects
to a WS/HTTP interface named after _SOCKSPEC_.
Generally when HTPP URI of the binder is *http://host:port*,
the WS/HTTP _SOCKSPEC_ is *http://host:port/api*.

When connecting to this interface, the 


## Connection to WSAPI interface

With the option *-d* (or *--direct*), *afb-client* connects
to a WSAPI interface named after _SOCKSPEC_.
Example of such _SOCKSPEC_ are:

- unix:@apiname
- unix:/run/platform/apis/ws/apiname
- tcp:host:port

The name *direct* means that the API is directly accessed and
then the API name must not be set but is implicit.

# OPTIONS

*-b, --break*
	Break connection just after the last event/call has been emitted.
	This option can be used for stressing the binder or when answer
	is irrevelant.

*-d, --direct*
	Direct API connection to WSAPI interface.

*-e, --echo*
	Echo inputs. Use this in batch for interleaving inputs
	and outputs.

*-h, --help*
	Display this help and exits.

*-H, --human*
	Display human readable JSON, spreading components on different lines.
	This is the opposite of option *--raw*.

*-k, --keep-running*
	Keep running until disconnect, even if input closed.

*-p, --pipe COUNT*
	Allow to pipe COUNT requests without waiting for answers.
	That means that a maximum of COUNT requests are pending
	without reply.

*-q, --quiet*
	Dont show informative lines beginning with *ON-*.
	Usefule for piping output to programs.

*-r, --raw*
	Raw output (default). This prints one line per reply or event
	without making JSON readable.
	This is the opposite of option *--human*.

*-s, --sync*
	Wait for the answer before sending the next query (like -p 1).

*-t, --token TOKEN*
	The token to use.

*-u, --uuid UUID*
	The identifier of session to use. This allow to recover a previously
	opened session.

*-v, --version*
	Print the version and exits.

*-w, --ws-maxlen VALUE*
	Set the maximum length of websocket payloads to the given value.
	By default, the maximum length is 1000000 (one million) bytes.
	This option can be used either to increase or decrese that limit.


# Connection to the binder and SOCKSPEC

The format of the specification _SOCKSPEC_ is:

- for TCP: *tcp:host:port/api*
- for unix: *unix:path/api*
- for abstract unix: *unix:@name/api* or *unix:@api*


# SEE ALSO

*afb-binder*(1), *afb-binding*(7)


# AUTHOR

José Bollo <jose.bollo@iot.bzh>

# LINKS

- The source code:        https://github.com/redpesk-core/afb-client
- Reporting bugs:         https://github.com/redpesk-core/afb-client/issues
- Redpesk documentation:  https://docs.redpesk.bzh
