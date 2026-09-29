/*
 * qrexec protocol v4 as upstream qubes-core-qrexec defines it (libqrexec/qrexec.h), for the parts this Windows port
 * uses. The Windows utils header this tree builds against stops at protocol v2: MSG_TRIGGER_SERVICE with a fixed
 * 64-byte service name. v3 made the service name variable-length (MSG_TRIGGER_SERVICE3) and raised the data chunk to
 * 64 KiB; v4 (Qubes 4.3) sends MSG_TRIGGER_SERVICE4, which adds a requested source domain. Upstream's Linux agent sends
 * ONLY the v4 trigger; this port does the same - consistency with the current code base, and Qubes 4.2 (a v3 daemon,
 * which exits on a v4 trigger) is not supported for guest-initiated calls.
 */
#pragma once
#include <windows.h>
#include <qrexec.h>

#define QREXEC_PROTOCOL_V4          4
#define MSG_TRIGGER_SERVICE4_ID     0x213   /* upstream MSG_TRIGGER_SERVICE4 = MSG_TRIGGER_SERVICE (0x210) + 3 */
#define MAX_SERVICE_NAME_LEN_V4     65000   /* upstream MAX_SERVICE_NAME_LEN: service name bytes INCLUDING the NUL */

/* the message id above is derived from MSG_TRIGGER_SERVICE; refuse to build if that base ever differs */
typedef char QrexecV4MsgBaseCheck[(MSG_TRIGGER_SERVICE == 0x210) ? 1 : -1];

/*
 * Fixed part of upstream's struct trigger_service_params4; on the wire it is followed by the NUL-terminated service name
 * (msg_header.len = sizeof(fixed part) + strlen(name) + 1). Spelled without a flexible array member (MSVC C4200).
 */
struct trigger_service_params4_fixed
{
    char source_domain[64];             /* NUL-terminated; empty = no requested source (as upstream's client sends) */
    char target_domain[64];             /* NUL-terminated */
    struct service_params request_id;   /* service request id, echoed back in MSG_SERVICE_CONNECT/REFUSED */
};

/* qrexec-client-vm -> qrexec-agent named-pipe request (internal; both ship in the same package):
 *   uint32 QREXEC_CLIENT_PIPE_MAGIC_V4, char target_domain[64], size_t service_name_size (incl. NUL),
 *   service name (UTF-8), size_t user_name_size, user name (WCHAR), size_t command_size, command line (WCHAR).
 * The magic makes a mismatched pair fail loudly instead of being misparsed. */
#define QREXEC_CLIENT_PIPE_MAGIC_V4 0x34564351u  /* "QCV4" */
