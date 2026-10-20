The OpenPMIx Wire Protocol
==========================

The PMIx Standard [#std]_ defines a C interface.  It does not define
the protocol between a PMIx client library and its server.  That
protocol is private to OpenPMIx, the library that Open MPI and other
programs link, and OpenPMIx does not document it.  This page
describes it, for the reader of a capture (see :doc:`tracing`), and
as the reference for the dissector that decodes one.

The protocol is nonetheless shaped by the standard.  Every value on
the wire is one of the standard's data types, with the standard's
type code; the structures it carries, such as a process identifier
or an info, are the standard's structures, field by field; and the
constants in them, such as a status, a scope, or a reserved rank,
are the standard's constants.  What OpenPMIx adds is the byte layout
of each type, the framing of a message, and the request and reply
sequence of each command.  This page gives those, and names the
standard's definition where one applies.

Scope
-----

This page describes the protocol as OpenPMIx 2.1 through 6.1
implement it.  OpenPMIx declares every series before 7.0 to be end
of life, with no further changes, so these versions can be described
as fixed.  OpenPMIx 7.0 is not described: it is a release candidate
at the time of writing, and what it adds can still change.

The facts here are from the OpenPMIx source, chiefly the v2.2,
v3.2, v4.2, v5.0, and v6.1 release branches, and were checked
against captures from OpenPMIx 5.0.7.  The footnotes link to the
source at the 5.0.7 tag.  OpenPMIx names the parts of the protocol
after its own components; this page uses those names, which also
appear in the connect request on the wire.

References to the standard are to version 5.0, the latest release
at the time of writing and the version current for OpenPMIx 5.0,
against which the captures were checked.  A section number such as
(standard 3.2.9) is a section of that document.

The page covers simple clients: processes that the server's host
launched, with an identity registered in advance.  It does not cover
tools, launchers, or schedulers, which connect with a different
handshake.  It describes every command a client can send, including
those flux-pmix does not implement, since a capture from a program
that uses one still has to be read.

Transport
---------

The client connects to the server over TCP, with the address it
finds in its environment.  The server writes the same address to
each of several variables, one for each encoding it offers::

   PMIX_SERVER_URI41=pmix-server.0;tcp4://127.0.0.1:57131
   PMIX_SERVER_URI4=pmix-server.0;tcp4://127.0.0.1:57131
   PMIX_SERVER_URI3=pmix-server.0;tcp4://127.0.0.1:57131
   PMIX_SERVER_URI21=pmix-server.0;tcp4://127.0.0.1:57131
   PMIX_SERVER_URI2=pmix-server.0;tcp4://127.0.0.1:57131

The value, in the ABNF of `RFC 5234
<https://www.rfc-editor.org/rfc/rfc5234>`_, is::

   uri       = server-id ";" address
   server-id = nspace "." rank
   address   = "tcp4://" ipv4 ":" port / "tcp6://" "[" ipv6 "]" ":" port

The client splits the server id at its last period, so the server's
namespace can hold periods.  The client does not negotiate
its encoding.  It looks for the variables of the encodings it has,
newest first, and uses the first one it finds. [#ptl]_

.. list-table::
   :header-rows: 1
   :widths: 16 44

   * - Client
     - Looks for, in order

   * - 2.1, 2.2
     - :envvar:`PMIX_SERVER_URI21`, :envvar:`PMIX_SERVER_URI2`

   * - 3.x
     - :envvar:`PMIX_SERVER_URI3`, then the above

   * - 4.0
     - :envvar:`PMIX_SERVER_URI4`, then the above

   * - 4.1, 4.2, 5.0
     - :envvar:`PMIX_SERVER_URI41`, then the above

   * - 6.0
     - :envvar:`PMIX_SERVER_URI51`, then the above

   * - 6.1
     - :envvar:`PMIX_SERVER_URI61`, then the above

Thus, a server selects a client's encoding by which variables it
sets.

One process can connect more than once with the same identity.  A
child that the client forks, and that calls :func:`PMIx_Init`,
connects again, and the server treats each connection on its own.

Connect request
---------------

The first message on a connection is the connect request, [#ptl]_
and the server's answer to it is the connect response.  Neither is
in the encoding that the rest of the protocol uses: the client has
not yet said which encoding that is.  Nothing in them is a pack
(see `Pack`_), no field has a type code, and no integer is a
varint.  Each integer is a fixed-width field, and they are not all
in one byte order:

* The header of the request is the client's ``pmix_ptl_hdr_t``
  copied as is, so its fields are in **host byte order**.  It is
  the only header in the protocol that is.
* The integers of the request body, and of the response, are 4
  bytes in **network byte order**.
* The native credential, inside the body, is the client's uid and
  gid copied as is, so in **host byte order**.

The header is the same 16 bytes as every other message (see
`Message header`_), with the index set to -1, the tag to
``UINT32_MAX``, and the length to that of the body.

The body is a sequence of fields.  In the table, a *string* is
NUL-terminated; *4 bytes* is a 4-byte unsigned integer in network
byte order; and *1 byte* is one byte.  These are not the data types
of the rest of this page.

.. list-table::
   :header-rows: 1
   :widths: 14 12 50

   * - Field
     - Type
     - Value

   * - security
     - string
     - The security method the client selected from
       :envvar:`PMIX_SECURITY_MODE`: munge, native, or none.

   * - credlen
     - 4 bytes
     - The length of the credential.

   * - cred
     - bytes
     - The credential.  For native it is the client's effective uid
       and gid, each 4 bytes in host byte order.  For munge it is a
       munge credential.  For none it is empty.

   * - flag
     - 1 byte
     - The process type.  A simple client sends 0.  Tools,
       launchers, and schedulers send other values and a different
       layout follows.

   * - nspace
     - string
     - The client's namespace (standard 3.2.2, ``pmix_nspace_t``).

   * - rank
     - 4 bytes
     - The client's rank (standard 3.2.3, ``pmix_rank_t``).

   * - version
     - string
     - The client library's version, such as 5.0.7, possibly with a
       suffix such as a1.

   * - encoding
     - string
     - The encoding the client selected: v21, v3, v4, v41, v51, or
       v61.

   * - buftype
     - 1 byte
     - The description mode (see `Pack`_): 1 for undescribed, 2 for
       described.

   * - storage
     - string
     - The data store the client selected: ds12, ds21, hash, or
       shmem2.

   * - info
     - rest
     - Optional.  A pack of one SIZE *n*, then a pack of *n* INFO,
       in the client's encoding.

A 2.x client ends the body after the version.  Clients from 3.0 on
send the info array, with ``pmix.ppid`` (the client's pid) and
``pmix.cmd.line``; clients from 4.0 on add ``pmix.euid``,
``pmix.egid``, ``pmix.ruid``, and ``pmix.rgid``.

The server looks up the namespace and rank, checks the credential,
and records the client's encoding, description mode, and store.  It
replies with no header: a 4-byte status in network byte order, and
if the status is 0, a 4-byte *peer index* in the same order, a small
integer that identifies the connection to the server.  The status is
one of the standard's status constants (standard 3.1.1), but it is
not a packed STATUS.  On any other status the server closes the
connection.
With munge the server also checks the uid and gid of the credential
against those registered for the rank.

Message header
--------------

After the handshake, every message is a 16-byte header and a
payload. [#header]_  The header fields are in network byte order,
whatever the encoding of the payload:

.. list-table::
   :header-rows: 1
   :widths: 10 10 50

   * - Offset
     - Type
     - Field

   * - 0
     - int32
     - Index.  The client sends its peer index.  The server sends
       its own index, which the client ignores.

   * - 4
     - uint32
     - Tag.

   * - 8
     - uint32
     - Length of the payload.

   * - 12
     - uint32
     - Padding, zero.  (The field is a ``size_t`` in the source, so
       the header is 12 bytes on a 32-bit platform.)

Tags 0, 1, and 2 are reserved: 0 carries event notifications from
the server, 1 heartbeats, and 2 I/O forwarding.  A client allocates
a tag for each request from 100 up, starting at 101, and the server
replies on the same tag.  More than one request can be outstanding,
and the server can reply in any order.

A payload is a sequence of packed values in the client's encoding
and description mode.  There is no length prefix on a sequence; a
receiver knows it has ended when the payload ends.

Encodings
---------

OpenPMIx has had six encodings, which it calls bfrops
modules. [#bfrops]_  Each server offers those it has, and a client
selects the newest it also has (see `Transport`_).

.. list-table::
   :header-rows: 1
   :widths: 10 46

   * - OpenPMIx
     - Encodings

   * - 2.1, 2.2
     - v21, v20, v12

   * - 3.x
     - v3, v21, v20, v12

   * - 4.0
     - v4, v3, v21, v20, v12

   * - 4.1, 4.2, 5.0
     - v41, v4, v3, v21, v20, v12

   * - 6.0
     - v51, v41, v4, v3, v21, v20, v12

   * - 6.1
     - v61, v51, v41, v4, v3, v21, v20, v12

There are only two wire formats.  **v21** and **v3** pack integers
at fixed width in network order.  **v4**, **v41**, **v51**, and
**v61** pack them as variable-length integers, and share one
implementation. [#base]_  Within each group, a later
encoding differs from an earlier one only in the data types it
defines (see `Data types`_).  v12 and v20 serve clients of 1.x and
2.0, which the servers of this page refuse, and are not described.

Pack
~~~~

Every value in a payload is written by a *pack* call, which writes
*n* values of one type *T*.  The connect request and response are
not payloads and have no packs (see `Connect request`_).  A pack
is:

1. In described mode only, the type code of INT32.
2. The count *n*, as an INT32.
3. In described mode only, the type code of *T*.
4. The *n* values, each in the layout of *T*.

A type code is a UINT16, the standard's ``pmix_data_type_t``
(standard 3.3).  A pack is what one call of the standard's
:func:`PMIx_Data_pack` (standard 11.3.1) writes; the standard
defines the call, with its count and type, but not what it writes.
The server sets the mode with :envvar:`PMIX_BFROP_BUFFER_TYPE` in
the client's environment, and the client announces it in the
connect request.  OpenPMIx servers use
undescribed mode.  In described mode, the type codes appear only at
this level, not inside the layouts of compound types.

A pack of *n* values is one count and *n* values.  It is not *n*
packs of one value.  The two are the same when *n* is 1, so a
decoder that confuses them decodes every one-element array and fails
on the first array of two.

A list inside a compound value, such as the keys of a QUERY or the
arguments of an APP, has no pack framing of its own: its count is a
field of the layout, and the elements follow it with no second
count.

Integers in v21 and v3
~~~~~~~~~~~~~~~~~~~~~~

An 8-bit integer is one byte.  A 16-, 32-, or 64-bit integer is 2,
4, or 8 bytes in network order.  SIZE, INT, UINT, and PID are
written at the sender's size for ``size_t``, ``int``, and ``pid_t``,
after a type code that gives that size: on 64-bit Linux, SIZE is the
code of UINT64 (15) and 8 bytes, and PID is the code of UINT32 (14)
and 4 bytes.

Integers in v4 through v61
~~~~~~~~~~~~~~~~~~~~~~~~~~

An 8-bit integer is one byte.  A 16-, 32-, or 64-bit integer is a
*varint*: the value in groups of 7 bits, least significant group
first, one group per byte, with bit 7 set on each byte that another
follows.  This is the unsigned LEB128 of DWARF and the varint of
Protocol Buffers, with one difference: at most 8 such bytes are
written, and if bits remain, a 9th byte holds bits 56 through 63 as
they are, with no continuation bit.

A signed integer is first sign-extended to 64 bits and mapped with
ZigZag, ``(v << 1) ^ (v >> 63)``, so that 0, -1, 1, -2, 2 become 0,
1, 2, 3, 4.  SIZE, INT, and UINT are varints with no type code.
PID keeps the type code of v3 before its varint.

Data types
~~~~~~~~~~

The types and their codes are the standard's ``pmix_data_type_t``
constants (standard 3.3), written here without the ``PMIX_``
prefix.  The standard defines each type's C structure, and OpenPMIx
packs that structure's fields in order, so the *Layout* column is
the standard's structure read as a sequence of types of this table.
Where the standard says a field is a pointer and a count, the layout
is the count and then the elements.  The standard leaves the codes
from 70 up to the implementation, and OpenPMIx assigns them; those
rows, and the layouts themselves, are OpenPMIx's.  Each row names
the structure's section of the standard where it has one.

.. list-table::
   :header-rows: 1
   :widths: 24 6 42

   * - Type
     - Code
     - Layout

   * - UNDEF
     - 0
     - Nothing.

   * - BOOL
     - 1
     - 1 byte, 0 or 1.

   * - BYTE, INT8, UINT8
     - 2, 7, 12
     - 1 byte.

   * - STRING
     - 3
     - INT32 length *L*, which counts the NUL, then *L* bytes.  A
       NULL string is length 0 and no bytes.

   * - SIZE
     - 4
     - See `Encodings`_.

   * - PID
     - 5
     - See `Encodings`_.

   * - INT, UINT
     - 6, 11
     - See `Encodings`_.

   * - INT16, UINT16
     - 8, 13
     - See `Encodings`_.

   * - INT32, UINT32
     - 9, 14
     - See `Encodings`_.

   * - INT64, UINT64
     - 10, 15
     - See `Encodings`_.

   * - FLOAT, DOUBLE
     - 16, 17
     - STRING, from ``printf("%f")``.

   * - TIMEVAL
     - 18
     - INT64 seconds, INT64 microseconds.

   * - TIME
     - 19
     - UINT64.

   * - STATUS
     - 20
     - INT32.  The values are the standard's status constants
       (standard 3.1.1), with the implementation's own below -1000.

   * - VALUE
     - 21
     - UINT16 type *t*, then one value of type *t*.  Nothing follows
       if *t* is UNDEF.  Standard 3.2.8, ``pmix_value_t``.

   * - PROC
     - 22
     - STRING namespace, UINT32 rank.  Standard 3.2.4,
       ``pmix_proc_t``.  The reserved ranks are the standard's:
       UINT32_MAX is ``PMIX_RANK_UNDEF`` and UINT32_MAX - 1 is
       ``PMIX_RANK_WILDCARD`` (standard 3.2.3).

   * - APP
     - 23
     - STRING cmd, INT argc, argc STRING, INT32 envc, envc STRING,
       STRING cwd, INT maxprocs, SIZE ninfo, ninfo INFO.  Standard
       12.2.5, ``pmix_app_t``.

   * - INFO
     - 24
     - STRING key, UINT32 flags, UINT16 type *t*, one value of type
       *t*.  Standard 3.2.9, ``pmix_info_t``; the flags are its
       ``pmix_info_directives_t`` (standard 3.2.10).

   * - PDATA
     - 25
     - PROC, STRING key, UINT16 type *t*, one value of type *t*.
       Standard 9.7.1, ``pmix_pdata_t``.

   * - BUFFER
     - 26
     - BYTE buffer type, SIZE length *L*, *L* bytes, which are a
       payload in the same encoding.

   * - BYTE_OBJECT
     - 27
     - SIZE length *L*, *L* bytes.  Standard 3.2.12,
       ``pmix_byte_object_t``.

   * - KVAL
     - 28
     - STRING key, VALUE.

   * - PERSIST, SCOPE, DATA_RANGE, COMMAND, PROC_STATE
     - 30, 32, 33, 34, 37
     - UINT8.  The values of SCOPE (standard 5.2.1.1), DATA_RANGE
       (standard 9.5.1), PERSIST (standard 9.5.2), and PROC_STATE
       (standard 3.2.5) are the standard's constants.  COMMAND is
       OpenPMIx's; see `Commands`_.

   * - INFO_DIRECTIVES
     - 35
     - UINT32.

   * - DATA_TYPE
     - 36
     - UINT16.

   * - PROC_INFO
     - 38
     - PROC, STRING hostname, STRING executable, PID, PROC_STATE.
       Standard 3.2.6, ``pmix_proc_info_t``.

   * - DATA_ARRAY
     - 39
     - UINT16 type *t*, SIZE *n*, then *n* values of type *t* if *n*
       is not 0 and *t* is not UNDEF.  Standard 3.2.13,
       ``pmix_data_array_t``.

   * - PROC_RANK
     - 40
     - UINT32.

   * - QUERY
     - 41
     - INT32 nkeys, nkeys STRING, SIZE nqual, nqual INFO.  Standard
       7.1.1, ``pmix_query_t``.

   * - ALLOC_DIRECTIVE
     - 43
     - UINT8.  Standard 13.1.4, ``pmix_alloc_directive_t``.

   * - IOF_CHANNEL
     - 45
     - UINT16, a bitmask.  From v3.

   * - ENVAR
     - 46
     - STRING name, STRING value, BYTE separator.  From v3.
       Standard 3.2.11, ``pmix_envar_t``.

   * - COORD
     - 47
     - UINT8 view, SIZE *n*, *n* UINT32.  From v4.

   * - REGATTR
     - 48
     - STRING name, STRING key, UINT16 type, INT32 *n*, *n* STRING
       descriptions.  From v4.

   * - REGEX
     - 49
     - A regex that begins with "pmix" is written as raw bytes to a
       NUL; any other is a STRING.  From v4.  The standard describes
       the prefix rule under :func:`PMIx_generate_regex` (standard
       17.2.1).

   * - JOB_STATE, LINK_STATE
     - 50, 51
     - UINT8.  From v4.

   * - PROC_CPUSET
     - 52
     - STRING, an hwloc list, or NULL if not bound.  From v4.

   * - GEOMETRY
     - 53
     - SIZE fabric, STRING uuid, STRING osname, SIZE *n*, *n* COORD.
       From v4.

   * - DEVICE_DIST
     - 54
     - STRING uuid, STRING osname, UINT64 type, UINT16 min, UINT16
       max.  From v4.

   * - ENDPOINT
     - 55
     - STRING uuid, STRING osname, SIZE *L*, *L* bytes.  From v4.

   * - TOPO
     - 56
     - STRING, the hwloc XML, then hwloc structs of hwloc's own
       layout.  From v4.

   * - DEVTYPE
     - 57
     - UINT64.  From v4.

   * - LOCTYPE
     - 58
     - UINT16.  From v4.

   * - COMPRESSED_BYTE_OBJECT
     - 59
     - As BYTE_OBJECT.  From v4.

   * - PROC_NSPACE
     - 60
     - STRING.  From v41.

   * - PROC_STATS, DISK_STATS, NET_STATS, NODE_STATS
     - 61–64
     - Monitoring statistics.  In v41 and v51 of 4.1 through 6.0;
       removed from both in 6.1.

   * - DATA_BUFFER
     - 65
     - As BYTE_OBJECT.  From v41.

   * - STOR_MEDIUM, STOR_ACCESS, STOR_PERSIST
     - 66–68
     - UINT64.  From v41.

   * - STOR_ACCESS_TYPE
     - 69
     - UINT16.  From v41.

   * - DEVICE
     - 70
     - STRING uuid, STRING osname, UINT64 type.  From v51.

   * - RESBLOCK_DIRECTIVE
     - 71
     - UINT8.  From v51.

   * - RESOURCE_UNIT
     - 72
     - UINT64 type, SIZE count.  From v51.

   * - NODE_PID
     - 73
     - STRING host, UINT32 node id, PID.  From v61.

   * - REGEX2
     - 74
     - STRING type, SIZE *L*, *L* bytes.  From v61.

A value of a type that an encoding does not have cannot be sent on
a connection that uses it: the client fails the operation with
``PMIX_ERR_UNKNOWN_DATA_TYPE`` (-16).  Thus, a 5.0 client connected with
v3 can send every type through ENVAR, and nothing after.

Layouts are not self-delimiting.  A decoder that does not know a
type cannot skip its value, and can decode no more of that payload.
It can still find the next message from the header.

The flags of an INFO are the standard's info directives (standard
3.2.10): 0x1 required, 0x2 end of array, 0x4 required and processed.
The standard reserves the top 16 bits for the implementation and
says nothing of 0x8, which OpenPMIx uses to mark a qualifier (see
`Qualified values`_).

Commands
--------

A request payload begins with a pack of one COMMAND.  The commands
are OpenPMIx's, one for each client call of the standard that
reaches the server; the standard has no notion of a command or its
code.  The codes [#cmd]_ have not changed from 2.1 through 6.1
except where a code was added:

.. list-table::
   :header-rows: 1
   :widths: 6 20 16 30

   * - Code
     - Command
     - From
     - Reply

   * - 0
     - job-info
     - 2.1
     - namespace and job information

   * - 1
     - abort
     - 2.1
     - status

   * - 2
     - commit
     - 2.1
     - status

   * - 3
     - fence
     - 2.1
     - status, when the fence completes

   * - 4
     - get
     - 2.1
     - status and data

   * - 5
     - finalize
     - 2.1
     - status

   * - 6, 7, 8
     - publish, lookup, unpublish
     - 2.1
     - status; lookup adds data

   * - 9
     - spawn
     - 2.1
     - status, namespace, job information

   * - 10, 11
     - connect, disconnect
     - 2.1
     - status, when the operation completes

   * - 12
     - notify
     - 2.1
     - status

   * - 13
     - register-events
     - 2.1
     - status

   * - 14
     - deregister-events
     - 2.1
     - none

   * - 15
     - query
     - 2.1
     - status and results

   * - 16
     - log
     - 2.1
     - status

   * - 17
     - allocate
     - 2.1
     - status and results

   * - 18
     - job-control
     - 2.1
     - status and results

   * - 19
     - monitor
     - 2.1
     - status

   * - 20, 21
     - get-credential, validate-credential
     - 3.0
     - status

   * - 22, 23, 29
     - iof-pull, iof-push, iof-deregister
     - 3.0
     - status

   * - 24–28
     - group-construct, group-join, group-invite, group-leave,
       group-destruct
     - 3.0
     - status; construct adds membership

   * - 30, 31
     - fabric-register, fabric-update
     - 4.0
     - status and results

   * - 32
     - compute-device-distances
     - 4.0
     - status and results

   * - 34
     - resource-block
     - 6.1
     - status and results

   * - 35
     - session-control
     - 6.0
     - status and results

   * - 36
     - request-sysinfo
     - 6.0
     - status and results

   * - 37, 38
     - resolve-peers, resolve-node
     - 5.0
     - status and results

   * - 39
     - gds-fallback
     - 6.1
     - status

Code 12 also carries event notifications from the server, on tag 0.

Every reply except job-info begins with a pack of one STATUS, whose
value is one of the standard's status constants (standard 3.1.1).
When the server cannot act on a request, including when it does not
implement the command, the reply is only the status, such as
``PMIX_ERR_NOT_SUPPORTED`` (-47).  Open MPI sends job-control at
startup; flux-pmix answers -47 and Open MPI continues.

The layouts below are from the client source [#client]_ of
OpenPMIx 6.1, with the differences of earlier versions noted.  Each is, in the main, the arguments of the
standard's client call packed in order, so the standard's
description of the call says what each field means; this page says
how it is carried.  They are written in a notation of this page, not
a formal grammar:

* Each line is one pack: a type, then the value or the name of the
  value.
* ``T[n]`` is one pack of *n* values of type *T*, where *n* is the
  value of an earlier line.
* ``[ ]`` around a line means the line is present only under the
  condition at its right.
* ``*`` after a line means the line repeats to the end of the
  payload.
* Text in parentheses is a note.

A failed request can be answered two ways.  When the server's host
has no handler for the command, the server replies with the status
alone.  When the host's handler runs and fails, the command's own
reply follows: where its layout has a count and a list after the
status, the count is sent, and is 0; other fields are present only
when the status is 0, unless the layout says otherwise.

The first group of commands is what an MPI program sends to
bootstrap.  The dissector decodes these.

job-info
~~~~~~~~

Sent in :func:`PMIx_Init` (standard 4.3), first after the
handshake::

   request:  COMMAND   0
   reply:    STRING    nspace
             KVAL      job information *

The nspace is the client's own, and the client rejects any other.
The KVAL sequence is described in `Job information`_.

commit
~~~~~~

Sent by :func:`PMIx_Commit` (standard 5.2.3) with the values the
client put since its last commit::

   request:  COMMAND   2
             [ SCOPE   1 (local)                          ]
             [ BUFFER  bucket: KVAL *                     ]
             [ SCOPE   2 (remote)                         ]
             [ BUFFER  bucket: KVAL *                     ]
   reply:    STATUS

The request has zero, one, or two scope-and-bucket pairs.  The
scopes are the standard's ``PMIX_LOCAL`` (1) and ``PMIX_REMOTE``
(2); a value put with ``PMIX_GLOBAL`` (3) is in both buckets.  A client sends a
commit with no buckets if it has nothing to commit.

fence
~~~~~

Sent by :func:`PMIx_Fence` (standard 8.1)::

   request:  COMMAND   3
             SIZE      nprocs
             PROC      procs[nprocs]
             SIZE      ninfo
             [ INFO    info[ninfo]       if ninfo > 0 ]
   reply:    STATUS

A client with no process list sends one PROC with its own namespace
and ``PMIX_RANK_WILDCARD``.  The info usually has ``pmix.collect``
(the standard's ``PMIX_COLLECT_DATA``, a bool), which asks that the
remote data of every participant be exchanged so that gets are then
served locally.

get
~~~

Sent by :func:`PMIx_Get` (standard 5.4) when the client does not
have the value::

   request:  COMMAND     4
             STRING      nspace
             PROC_RANK   rank
             SIZE        ninfo
             [ INFO      info[ninfo]     if ninfo > 0 ]
             STRING      key
   reply:    STATUS
             [ BYTE_OBJECT  proc-data *   if status == 0 ]

A 2.x client does not send the key.  Each proc-data holds, in the
client's encoding, a pack of one PROC and then a pack of one KVAL
for each value.  A rank of ``PMIX_RANK_UNDEF`` in that PROC means
the requested process; ``PMIX_RANK_WILDCARD`` means the namespace.

The server defers the reply until it has the data, unless the info
has ``pmix.immediate`` (``PMIX_IMMEDIATE``) set, in which case it
replies at once with ``PMIX_ERR_NOT_FOUND`` (-46).

finalize
~~~~~~~~

Sent by :func:`PMIx_Finalize` (standard 4.4)::

   request:  COMMAND   5
   reply:    STATUS

The client waits for the reply for a limited time, then closes.

abort
~~~~~

Sent by :func:`PMIx_Abort` (standard 12.1.1)::

   request:  COMMAND   1
             STATUS    exit status
             STRING    message
             SIZE      nprocs
             [ PROC    procs[nprocs]     if nprocs > 0 ]
   reply:    STATUS

An empty process list means the caller's whole namespace.

register-events and deregister-events
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Open MPI registers event handlers at startup, so every bootstrap
has these::

   request:  COMMAND   13
             SIZE      ncodes
             [ STATUS  codes[ncodes]     if ncodes > 0 ]
             SIZE      ninfo
             [ INFO    info[ninfo]       if ninfo > 0  ]
   reply:    STATUS

   request:  COMMAND   14
             STATUS    code *            (one pack per code)

There is no reply to deregister-events.  Its codes are one pack
each, not one pack of *n*.  The code ``INT32_MIN`` means the default
handler, which covers all codes.

notify and events
~~~~~~~~~~~~~~~~~

A client sends :func:`PMIx_Notify_event` (standard 10.1.8) as a
request.  The server
sends an event to a client as a message on tag 0, with the same
command code, and the client does not reply::

   request:  COMMAND     12
             STATUS      event code
             DATA_RANGE  range
             SIZE        ninfo
             [ INFO      info[ninfo]     if ninfo > 0 ]
   reply:    STATUS

   event:    COMMAND     12
             STATUS      event code
             PROC        source
             SIZE        ninfo
             [ INFO      info[ninfo]     if ninfo > 0 ]

job-control
~~~~~~~~~~~

Sent by :func:`PMIx_Job_control` (standard 13.2.1).  Open MPI 5.0
sends one at startup to register its shared memory segment for
cleanup::

   request:  COMMAND   18
             SIZE      ntargets
             [ PROC    targets[ntargets]    if ntargets > 0 ]
             SIZE      ndirs
             [ INFO    directives[ndirs]    if ndirs > 0    ]
   reply:    STATUS
             [ SIZE    ninfo                               ]
             [ INFO    results[ninfo]       if ninfo > 0   ]

The directives include ``pmix.jctrl.kill``, ``pmix.jctrl.sig``,
``pmix.jctrl.term``, and ``pmix.reg.cleanup``, which Open MPI uses.

The commands that follow are those of dynamic process and resource
management.  flux-pmix does not implement them at the time of
writing; they are here for when it does, and for the reader of a
capture from a program that uses them.

publish, lookup, and unpublish
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Sent by :func:`PMIx_Publish`, :func:`PMIx_Lookup`, and
:func:`PMIx_Unpublish` (standard 9.1, 9.6, and 9.9), which MPI uses for
:func:`MPI_Publish_name` and its relatives::

   request:  COMMAND   6 (publish)
             UINT32    uid
             SIZE      ninfo
             [ INFO    info[ninfo]       if ninfo > 0 ]
   reply:    STATUS

   request:  COMMAND   7 (lookup)
             UINT32    uid
             SIZE      nkeys
             STRING    key *             (one pack per key)
             SIZE      ninfo
             [ INFO    info[ninfo]       if ninfo > 0 ]
   reply:    STATUS
             [ SIZE    ndata                            ]
             [ PDATA   data[ndata]       if ndata > 0  ]

   request:  COMMAND   8 (unpublish)
             UINT32    uid
             SIZE      nkeys
             STRING    key *             (one pack per key)
             SIZE      ninfo
             [ INFO    info[ninfo]       if ninfo > 0 ]
   reply:    STATUS

The uid is the client's own effective uid.  In a publish, each info
is a key and the value to publish; in the others, the info holds
directives such as ``pmix.range`` and ``pmix.wait``.  The keys of
a lookup or unpublish are one pack each, not one pack of *n*.

spawn
~~~~~

Sent by :func:`PMIx_Spawn` (standard 12.2.1)::

   request:  COMMAND   9
             SIZE      ninfo
             [ INFO    job info[ninfo]   if ninfo > 0 ]
             SIZE      napps
             [ APP     apps[napps]       if napps > 0 ]
   reply:    STATUS
             STRING    nspace
             [ KVAL    job information *   if status == 0 ]

The server replies when the new job is launched, with
its namespace and its job information in the form of a job-info
reply.  The new processes connect as clients of that namespace, and
their job information should have ``pmix.spawned`` and
``pmix.parent``, which MPI uses to know it was spawned.

connect and disconnect
~~~~~~~~~~~~~~~~~~~~~~

Sent by :func:`PMIx_Connect` and :func:`PMIx_Disconnect` (standard
12.3.1 and 12.3.3), which MPI uses to join jobs that were launched or spawned
separately::

   request:  COMMAND   10 (connect) or 11 (disconnect)
             SIZE      nprocs
             PROC      procs[nprocs]
             SIZE      ninfo
             [ INFO    info[ninfo]       if ninfo > 0 ]
   reply:    STATUS
             [ BYTE_OBJECT  job data *   connect only ]

Both are collective: the server replies when every listed process
has made the same request.  On a connect, the reply has one job data
object for each other namespace in the set.  Its bytes are a pack of
one STRING nspace and then that namespace's job information as KVAL
packs, in the client's encoding.  After a connect, a client can get
values of processes in the connected namespaces.

process groups
~~~~~~~~~~~~~~

Sent by :func:`PMIx_Group_construct`, :func:`PMIx_Group_leave`, and
:func:`PMIx_Group_destruct` (standard 14.2.6, 14.2.14, and 14.2.8).  Open MPI 5.0 and later build MPI
Sessions on them (see below).  The group commands are in clients
from 4.0::

   request:  COMMAND   24 (construct)
             STRING    group id
             SIZE      nprocs
             PROC      procs[nprocs]
             SIZE      ninfo
             [ INFO    info[ninfo]         if ninfo > 0    ]
   reply:    STATUS
             [ SIZE    nmembers                             ]
             [ PROC    members[nmembers]   if nmembers > 0 ]
             [ BOOL    has context id      6.0 and later   ]
             [ SIZE    context id          if it has one   ]
             [ SIZE    ninfo               6.1 and later   ]
             [ INFO    group info[ninfo]   if ninfo > 0    ]

   request:  COMMAND   27 (leave)
             STRING    group id
             SIZE      ninfo
             [ INFO    info[ninfo]         if ninfo > 0 ]
   reply:    STATUS

   request:  COMMAND   28 (destruct)
             STRING    group id
             SIZE      nmembers
             PROC      members[nmembers]
             SIZE      ninfo
             [ INFO    info[ninfo]         if ninfo > 0 ]
   reply:    STATUS

Construct and destruct are collective over the listed processes.  A
4.x or 5.0 client reads the context id without a BOOL before it,
and the server sends the field whether or not one was requested,
since the client cannot otherwise tell.  The server assigns a
context id, unique across the system, when the request's info has
``pmix.grp.actxid`` set.  A 6.1 client also takes the group
information of each member, which it requested with
``pmix.grp.info``; an earlier client gets it with get, as qualified
values (see `Qualified values`_) of each member, qualified by
``pmix.grp.ctxid``.

:func:`PMIx_Group_invite` and :func:`PMIx_Group_join` are built on
events in the client and do not use commands 25 and 26.

Open MPI 5.0 implements :func:`MPI_Comm_create_from_group` and
:func:`MPI_Intercomm_create_from_groups` with these: each member
constructs a group named for the new communicator, with
``pmix.grp.actxid`` and a ``pmix.grp.info`` that holds
``pmix.grp.lclid``, its local communicator index; takes the context
id as the base of the new communicator's id, and fails if there is
none; and then destructs the group.  Later, a member that needs a
peer's local index gets ``pmix.grp.lclid`` from that peer, qualified
by ``pmix.grp.ctxid``.  Thus, a construct and a destruct are on the
path of each such communicator.  :func:`MPI_Session_init` also queries
the server for process sets (see `query`_).

query
~~~~~

Sent by :func:`PMIx_Query_info` (standard 7.1.2) for what the client
cannot answer from its own data::

   request:  COMMAND   15
             SIZE      nqueries
             QUERY     queries[nqueries]
   reply:    STATUS
             [ SIZE    ninfo                            ]
             [ INFO    results[ninfo]     if ninfo > 0 ]

Open MPI 5.0 queries ``pmix.qry.psetnum`` and
``pmix.qry.psets``, qualified by ``pmix.qry.rfsh`` so that the
request reaches the server, and parses ``pmix.qry.psets`` as a
comma-separated STRING, though the standard says a DATA_ARRAY.

allocate
~~~~~~~~

Sent by :func:`PMIx_Allocation_request` (standard 13.1.1)::

   request:  COMMAND           17
             ALLOC_DIRECTIVE   directive
             SIZE              ninfo
             [ INFO            info[ninfo]       if ninfo > 0 ]
   reply:    STATUS
             [ SIZE            ninfo                          ]
             [ INFO            results[ninfo]    if ninfo > 0 ]

The directive is the standard's ``pmix_alloc_directive_t``: 1 new,
2 extend, 3 release, 4 reacquire, or 5 cancel, which OpenPMIx adds.  The info describes
the request, such as ``pmix.alloc.nnodes`` and ``pmix.alloc.time``;
the results describe the grant, such as ``pmix.alloc.id``.  The
server can reply before the grant and report the outcome later as
an event.

log
~~~

Sent by :func:`PMIx_Log` (standard 13.4.1)::

   request:  COMMAND   16
             TIME      timestamp          server 3.0 or later
             SIZE      ndata
             [ INFO    data[ndata]        if ndata > 0 ]
             SIZE      ndirs
             [ INFO    directives[ndirs]  if ndirs > 0 ]
   reply:    STATUS

monitor
~~~~~~~

Sent by :func:`PMIx_Process_monitor` (standard 13.3.1)::

   request:  COMMAND   19
             INFO      monitor
             STATUS    error code
             SIZE      ndirs
             [ INFO    directives[ndirs]  if ndirs > 0 ]
   reply:    STATUS
             [ SIZE    ninfo                           ]
             [ INFO    results[ninfo]     if ninfo > 0 ]

credentials
~~~~~~~~~~~

Sent by :func:`PMIx_Get_credential` and
:func:`PMIx_Validate_credential` (standard 16.1.1 and 16.2.1),
from 3.0::

   request:  COMMAND       20 (get)
             SIZE          ninfo
             [ INFO        info[ninfo]        if ninfo > 0 ]
   reply:    STATUS
             [ BYTE_OBJECT credential                       ]
             [ SIZE        ninfo                            ]
             [ INFO        info[ninfo]        if ninfo > 0 ]

   request:  COMMAND       21 (validate)
             BYTE_OBJECT   credential
             SIZE          ndirs
             [ INFO        directives[ndirs]  if ndirs > 0 ]
   reply:    STATUS
             [ SIZE        ninfo                            ]
             [ INFO        info[ninfo]        if ninfo > 0 ]

I/O forwarding
~~~~~~~~~~~~~~

Sent by :func:`PMIx_IOF_pull`, :func:`PMIx_IOF_push`, and
:func:`PMIx_IOF_deregister` (standard 18.5.7 through 18.5.9), from
3.0.  The forwarded data itself
goes on tag 2::

   request:  COMMAND       22 (pull)
             SIZE          nprocs
             [ PROC        procs[nprocs]      if nprocs > 0 ]
             SIZE          ndirs
             [ INFO        directives[ndirs]  if ndirs > 0  ]
             IOF_CHANNEL   channels
             SIZE          local id
   reply:    STATUS
             [ SIZE        remote id                        ]

   request:  COMMAND       23 (push)
             SIZE          nprocs
             [ PROC        targets[nprocs]    if nprocs > 0 ]
             SIZE          ndirs
             [ INFO        directives[ndirs]  if ndirs > 0  ]
             [ BYTE_OBJECT data               if any        ]
   reply:    STATUS

   request:  COMMAND       29 (deregister)
             SIZE          ndirs
             [ INFO        directives[ndirs]  if ndirs > 0  ]
             SIZE          remote id
   reply:    STATUS

   on tag 2: PROC          source
             IOF_CHANNEL   channel
             SIZE          remote id
             SIZE          ninfo
             [ INFO        info[ninfo]        if ninfo > 0  ]
             BYTE_OBJECT   data

fabric and devices
~~~~~~~~~~~~~~~~~~

Sent by :func:`PMIx_Fabric_register`, :func:`PMIx_Fabric_update`,
and :func:`PMIx_Compute_distances` (standard 15.4.1, 15.4.3, and
12.4.5), from 4.0::

   request:  COMMAND       30 (fabric-register)
             SIZE          ndirs
             [ INFO        directives[ndirs]  if ndirs > 0 ]
   reply:    STATUS
             [ SIZE        ninfo                           ]
             [ INFO        fabric info[ninfo] if ninfo > 0 ]

   request:  COMMAND       31 (fabric-update)
             SIZE          fabric index
   reply:    as fabric-register

   request:  COMMAND       32 (compute-device-distances)
             TOPO          topology
             PROC_CPUSET   cpuset
             SIZE          ninfo
             [ INFO        info[ninfo]        if ninfo > 0 ]
   reply:    STATUS
             [ SIZE        ndist                           ]
             [ DEVICE_DIST distances[ndist]   if ndist > 0 ]

resolve
~~~~~~~

Sent by :func:`PMIx_Resolve_peers` and :func:`PMIx_Resolve_nodes`
(standard 7.2), from 5.0::

   request:  COMMAND   37 (resolve-peers)
             STRING    node name
             STRING    nspace
   reply:    STATUS
             [ SIZE    nprocs                           ]
             [ PROC    procs[nprocs]      if nprocs > 0 ]

   request:  COMMAND   38 (resolve-node)
             STRING    nspace
   reply:    STATUS
             [ STRING  node list                        ]

session-control
~~~~~~~~~~~~~~~

Sent by :func:`PMIx_Session_control`, from 6.0, by which a scheduler
or a job pauses, resumes, or ends a session::

   request:  COMMAND   35
             UINT32    session id
             SIZE      ndirs
             [ INFO    directives[ndirs]  if ndirs > 0 ]
   reply:    STATUS
             [ SIZE    ninfo                           ]
             [ INFO    results[ninfo]     if ninfo > 0 ]

resource-block
~~~~~~~~~~~~~~

Sent by :func:`PMIx_Resource_block`, from 6.1.  Its types are in v51
and later, so a 6.1 client connected with v41 cannot send it::

   request:  COMMAND             34
             RESBLOCK_DIRECTIVE  directive
             STRING              block name
             SIZE                nunits
             [ RESOURCE_UNIT     units[nunits]      if nunits > 0 ]
             SIZE                ninfo
             [ INFO              info[ninfo]        if ninfo > 0  ]
   reply:    STATUS
             [ SIZE              ninfo                            ]
             [ INFO              results[ninfo]     if ninfo > 0  ]

gds-fallback
~~~~~~~~~~~~

Sent by a 6.1 client when its data store fails to attach to the
server's, to ask for another::

   request:  COMMAND   39
             STRING    module name
   reply:    STATUS

Code 36, request-sysinfo, is defined in 6.0 but no client sends it.

Job information
---------------

The job-info reply is the client's main source of information about
the job.  What it holds depends on the data store [#gds]_ that the
server selected for the client (see :doc:`openpmix_interop`).

With **hash**, the reply holds the job information itself, as a
sequence of KVAL.  The keys and their meanings are the standard's
reserved keys (standard 6), which it sorts into *data realms*:
session, job, application, node, and process (standard 6.1).  The
standard says which keys of each realm a host must provide and which
it may (standard 17.2.3, :func:`PMIx_server_register_nspace`), and
OpenPMIx derives more from what the host gives it, so what a given
server sends is not fixed by the protocol.  What the protocol fixes is how the realms are
carried.  A key that is not one of the containers below is
job-realm information, which the client files under the wildcard
rank:

.. list-table::
   :header-rows: 1
   :widths: 18 12 40

   * - Key
     - Type
     - Holds

   * - pmix.ssn.arr
     - DATA_ARRAY of INFO
     - Session-realm information.  The standard requires
       pmix.session.id in the array.

   * - pmix.app.arr
     - DATA_ARRAY of INFO
     - Application-realm information.  The standard requires
       pmix.appnum in the array.

   * - pmix.node.arr
     - DATA_ARRAY of INFO
     - Node-realm information.  The standard requires pmix.hname or
       pmix.nodeid in the array.

   * - pmix.pblob
     - BYTE_OBJECT
     - Process-realm information for one rank: a pack of one
       PROC_RANK, then a pack of one KVAL for each value, in the
       client's encoding.

The first three are the standard's own attributes,
``PMIX_SESSION_INFO_ARRAY``, ``PMIX_APP_INFO_ARRAY``, and
``PMIX_NODE_INFO_ARRAY`` (standard 17.2.3.1), with its rules for
what identifies the array.  ``pmix.pblob`` is OpenPMIx's: the
standard carries process-realm information in ``pmix.pdata``
(``PMIX_PROC_INFO_ARRAY``), an array of INFO with ``pmix.rank`` in
it, and ``pmix.pblob`` is a packed form of the same that OpenPMIx
sends in its place.  The standard also lets the arrays nest, a
job array holding node arrays for example; OpenPMIx flattens them
before sending.

With **ds12** or **ds21**, the server writes the job information to
a shared memory segment before the client starts, and the client
finds the segment by :envvar:`PMIX_DSTORE_21_BASE_PATH` (or
:envvar:`PMIX_DSTORE_12_BASE_PATH`) in its environment.  The job-info reply is only the nspace.

With **shmem2**, the server writes the job information to a segment
when the first client connects.  The reply holds three keys, each a
BYTE_OBJECT: two PMIX_GDS_SHMEM2_SEG_BLOB, which describe the
segment, and PMIX_GDS_SHMEM2_KIDX, which is a copy of the server's
attribute dictionary, about 185 KB.

Clients before 3.1.5 take node information in an older form: each
node's array under a key that is the node's hostname, and the keys
of the client's own node repeated at job level.  A 3.2 or later
server sends that form to such a client.

Qualified values
~~~~~~~~~~~~~~~~

A value that applies only under some condition, such as within one
process group, is a KVAL with key ``pmix.qual.val`` and a
DATA_ARRAY of INFO as its value.  The first INFO is the key and
value; each other INFO is a qualifier, with flag 0x8 set.  A client
that wants such a value puts the qualifiers, with that flag, in the
info of its get request.  This is an OpenPMIx convention.  The
standard uses qualifiers as directives to a call, such as the info
of :func:`PMIx_Get` (standard 5.4), and does not define this key,
the flag, or any way to store a value under qualifiers.

Version summary
---------------

.. list-table::
   :header-rows: 1
   :widths: 12 10 50

   * - Client
     - Encoding
     - Protocol differences

   * - 2.1, 2.2
     - v21
     - Ends the connect body after the version.  Does not send the
       key in get.

   * - 3.0, 3.1
     - v3
     - Sends the connect info array.  Sends the key in get.  Takes
       the older node information form before 3.1.5.

   * - 3.2
     - v3
     - As 3.1.5.

   * - 4.0
     - v4
     - Adds uid and gid to the connect info.  Adds types 47–59.

   * - 4.1, 4.2
     - v41
     - Adds types 60–69.

   * - 5.0
     - v41
     - As 4.2.  Adds commands 37 and 38.

   * - 6.0
     - v51
     - Adds types 70–72 and commands 35 and 36.

   * - 6.1
     - v61
     - Removes types 61–64.  Adds types 73 and 74 and commands 34
       and 39.

.. rubric:: References

.. [#std] PMIx Standard, version 5.0, PMIx Administrative Steering
   Committee, 2023.
   https://github.com/pmix/pmix-standard/releases/download/v5.0/pmix-standard-v5.0.pdf

.. [#ptl] The transport layer, including the connect handshake:
   https://github.com/openpmix/openpmix/tree/v5.0.7/src/mca/ptl

.. [#header] The message header, ``pmix_ptl_hdr_t``:
   https://github.com/openpmix/openpmix/blob/v5.0.7/src/mca/ptl/ptl_types.h

.. [#bfrops] The encodings, one module per directory:
   https://github.com/openpmix/openpmix/tree/v5.0.7/src/mca/bfrops

.. [#base] The pack and unpack routines the v4 and later encodings
   share:
   https://github.com/openpmix/openpmix/tree/v5.0.7/src/mca/bfrops/base

.. [#cmd] The command codes, ``pmix_cmd_t``:
   https://github.com/openpmix/openpmix/blob/v5.0.7/src/include/pmix_globals.h

.. [#client] The client side of each command:
   https://github.com/openpmix/openpmix/tree/v5.0.7/src/client and
   https://github.com/openpmix/openpmix/tree/v5.0.7/src/common

.. [#gds] The data stores, one module per directory:
   https://github.com/openpmix/openpmix/tree/v5.0.7/src/mca/gds
