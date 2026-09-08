#ifndef NRFCLAW_VERSION_H
#define NRFCLAW_VERSION_H

/* Stage 10.2.3a firmware identity.
 * Keep the compact numeric fields stable on the wire.  A build system may
 * override NRFCLAW_FW_BUILD and NRFCLAW_FW_GIT32 with -D flags.
 */
#define NRFCLAW_FW_VERSION_MAJOR 10U
#define NRFCLAW_FW_VERSION_MINOR 2U
#define NRFCLAW_FW_VERSION_PATCH 3U
#define NRFCLAW_FW_PRERELEASE    1U  /* 0=release, 1=stage-a/dev */

#ifndef NRFCLAW_FW_BUILD
#define NRFCLAW_FW_BUILD         1U
#endif

#ifndef NRFCLAW_FW_GIT32
#define NRFCLAW_FW_GIT32         0x00000000UL
#endif

#define NRFCLAW_NDP_VERSION      1U
#define NRFCLAW_VM_ABI_VERSION   1U
#ifndef NRFCLAW_BOARD_TYPE
#define NRFCLAW_BOARD_TYPE       0U
#endif
#define NRFCLAW_SD_FAMILY        132U
#define NRFCLAW_SD_VERSION_MAJOR 7U
#define NRFCLAW_SD_VERSION_MINOR 2U
#define NRFCLAW_SD_VERSION_PATCH 0U
#ifndef NRFCLAW_HW_REV_MAJOR
#define NRFCLAW_HW_REV_MAJOR     0U
#endif
#ifndef NRFCLAW_HW_REV_MINOR
#define NRFCLAW_HW_REV_MINOR     0U
#endif

#endif
