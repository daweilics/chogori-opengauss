/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/globals.c
 *
 * NOTES
 *	  Globals used all over the place should be declared here and not
 *	  in other modules.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "storage/backendid.h"
#include "utils/formatting.h"
#include "utils/atomic.h"
#include "replication/slot.h"

THR_LOCAL ProtocolVersion FrontendProtocol = PG_PROTOCOL_LATEST;

THR_LOCAL volatile bool InterruptPending = false;

THR_LOCAL char my_exec_path[MAXPGPATH]; /* full path to my executable */

/*
 * Hook on object accesses.  This is intended as infrastructure for security
 * and logging plugins.
 */
THR_LOCAL object_access_hook_type object_access_hook = NULL;

uint8 ce_cache_refresh_type = 0;

/*
 * standalone backend).  IsUnderPostmaster is true in postmaster child
 * processes.  Note that "child process" includes all children, not only
 * regular backends.  These should be set correctly as early as possible
 * in the execution of a process, so that error handling will do the right
 * things if an error should occur during process initialization.
 *
 * These are initialized for the bootstrap/standalone case.
 */
THR_LOCAL bool IsUnderPostmaster = false;

volatile ThreadId PostmasterPid = 0;
bool IsPostmasterEnvironment = false;
bool open_join_children = true;
bool will_shutdown = false;

/* hard-wired binary version number */
const uint32 GRAND_VERSION_NUM = 92421;

const uint32 HINT_ENHANCEMENT_VERSION_NUM = 92359;
const uint32 MATVIEW_VERSION_NUM = 92213;
const uint32 PARTIALPUSH_VERSION_NUM = 92087;
const uint32 SUBLINKPULLUP_VERSION_NUM = 92094;
const uint32 PREDPUSH_VERSION_NUM = 92096;
const uint32 GTMLITE_VERSION_NUM = 92110;
const uint32 EXECUTE_DIRECT_ON_MULTI_VERSION_NUM = 92140;
const uint32 FIX_PBE_CUSTOME_PLAN_BUG_VERSION_NUM = 92148;
const uint32 FUNCNAME_PUSHDOWN_VERSION_NUM = 92202;
const uint32 STP_SUPPORT_COMMIT_ROLLBACK = 92219;
const uint32 SUPPORT_GPI_VERSION_NUM = 92257;
const uint32 PRIVS_VERSION_NUM = 92259;
const uint32 EXTRA_SLOT_VERSION_NUM = 92260;
const uint32 RANGE_LIST_DISTRIBUTION_VERSION_NUM = 92272;
const uint32 BACKUP_SLOT_VERSION_NUM = 92282;
const uint32 ML_OPT_MODEL_VERSION_NUM = 92284;
const uint32 FIX_SQL_ADD_RELATION_REF_COUNT = 92291;
const uint32 INPLACE_UPDATE_WERSION_NUM = 92350;
const uint32 GENERATED_COL_VERSION_NUM = 92355;
const uint32 SEGMENT_PAGE_VERSION_NUM = 92360;
const uint32 COMMENT_PROC_VERSION_NUM = 92372;
const uint32 CLIENT_ENCRYPTION_PROC_VERSION_NUM = 92383;
const uint32 DECODE_ABORT_VERSION_NUM = 92386;
const uint32 COPY_TRANSFORM_VERSION_NUM = 92394;
const uint32 COMMENT_PCT_TYPE_VERSION_NUM = 92396;
const uint32 TDE_VERSION_NUM = 92407;
/* Version number of the guc parameter backend_version added in V500R001C20 */
const uint32 V5R1C20_BACKEND_VERSION_NUM = 92305;
/* Version number starting from V500R002C00 */
const uint32 V5R2C00_START_VERSION_NUM = 92350;
/* Version number of the guc parameter backend_version added in V500R002C00 */
const uint32 V5R2C00_BACKEND_VERSION_NUM = 92412;

const uint32 ANALYZER_HOOK_VERSION_NUM = 92420;
const uint32 SUPPORT_HASH_XLOG_VERSION_NUM = 92420;

/* This variable indicates wheather the instance is in progress of upgrade as a whole */
uint32 volatile WorkingGrandVersionNum = GRAND_VERSION_NUM;

const uint32 TWOPHASE_FILE_VERSION = 92414;

bool InplaceUpgradePrecommit = false;

#ifdef PGXC
bool useLocalXid = false;
#endif

/*
 *     EarlyBindingTLSVariables
 *         Bind static variables to another static TLS variable's address.
 *
 *	   This is needed because of the inability of the compiler: compiler
 *	   complains if you intialize a static TLS variable as another TLS
 *	   variable's address. So we do it for compiler in the earilest stage
 *	   of thread starting once.
 */
void EarlyBindingTLSVariables(void)
{
    static THR_LOCAL bool fDone = false;

    /* This shall be done only once per thread */
    if (!fDone) {
        /* Init number formatting cache */
        Init_NUM_cache();
        /* Init transaction state */
        InitCurrentTransactionState();
    }

    fDone = true;
}
