#ifndef NRFCLAW_NINALINK_COMMAND_H
#define NRFCLAW_NINALINK_COMMAND_H

/*
 * NinaLink v1 application command ABI, frozen at B4.7.
 *
 * Keep these definitions in a dedicated shared header so bridge and node do
 * not depend on whether the base NinaLink message IDs are represented by an
 * enum or by preprocessor macros in a particular revision.
 */
#define NRFCLAW_NINALINK_MSG_COMMAND         0x31U
#define NRFCLAW_NINALINK_MSG_COMMAND_RESULT  0x32U

#define NRFCLAW_NINALINK_COMMAND_ECHO_U32    0x0001U

#define NRFCLAW_NINALINK_COMMAND_STATUS_OK           0U
#define NRFCLAW_NINALINK_COMMAND_STATUS_UNSUPPORTED  1U
#define NRFCLAW_NINALINK_COMMAND_STATUS_BAD_ARGS     2U
#define NRFCLAW_NINALINK_COMMAND_STATUS_EXEC_FAILED  3U

#endif
