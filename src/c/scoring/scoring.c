/**
 * Copyright 2018-2019 ZomboDB, LLC
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scoring.h"

#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "parser/parse_func.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(zdb_score);


typedef struct ZDBScoringSupportData {
	Oid  heapOid;
	List *callbacks;
	List *callback_data;
} ZDBScoringSupportData;

extern List *currentQueryStack;

static List *scoreEntries = NULL;

/*lint -esym 715,event,arg */
static void scoring_cleanup_callback(XactEvent event, void *arg) {
	scoring_support_cleanup();
}

void scoring_support_init(void) {
	RegisterXactCallback(scoring_cleanup_callback, NULL);
}

void scoring_support_cleanup(void) {
	scoreEntries = NULL;
}

HTAB *scoring_create_lookup_table(MemoryContext memoryContext, char *name) {
	HASHCTL ctl;

	memset(&ctl, 0, sizeof(HASHCTL));
	ctl.hcxt      = memoryContext;
	ctl.keysize   = sizeof(ZDBScoreKey);
	ctl.entrysize = sizeof(ZDBScoreEntry);
	ctl.hash      = tag_hash;

	return hash_create(name, 10000, &ctl, HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
}

void scoring_register_callback(Oid heapOid, score_lookup_callback callback, void *callback_data, MemoryContext memoryContext) {
	MemoryContext         oldContext = MemoryContextSwitchTo(memoryContext);
	ZDBScoringSupportData *entry;
	ListCell              *lc;

	foreach(lc, scoreEntries) {
		ZDBScoringSupportData *existing = lfirst(lc);
		if (heapOid == existing->heapOid) {
			/* we already have an entry for this name, so add another callback */
			existing->callbacks     = lappend(existing->callbacks, callback);
			existing->callback_data = lappend(existing->callback_data, callback_data);

			MemoryContextSwitchTo(oldContext);
			return;
		}
	}

	/* create a new entry */
	entry = palloc0(sizeof(ZDBScoringSupportData));
	entry->heapOid       = heapOid;
	entry->callbacks     = lappend(entry->callbacks, callback);
	entry->callback_data = lappend(entry->callback_data, callback_data);

	scoreEntries = lappend(scoreEntries, entry);
	MemoryContextSwitchTo(oldContext);
}

static float4 scoring_lookup_score(Oid heapOid, ItemPointer ctid) {
	ListCell *lc, *lc2, *lc3;
	float4   score = 0.0;

	foreach(lc, scoreEntries) {
		ZDBScoringSupportData *entry = lfirst(lc);

		if (heapOid == entry->heapOid) {
			forboth(lc2, entry->callbacks, lc3, entry->callback_data) {
				score_lookup_callback callback = lfirst(lc2);
				void                  *arg     = lfirst(lc3);

				score += callback(ctid, arg);
			}
		}
	}

	return score;
}

Datum zdb_score(PG_FUNCTION_ARGS) {
	/* zdb.score() is not defined as strict, so we need to null-check our argument */
	if (PG_ARGISNULL(0)) {
		PG_RETURN_FLOAT4(0.0);
	} else {
		ItemPointer ctid      = (ItemPointer) PG_GETARG_POINTER(0);
		FuncExpr    *funcExpr = (FuncExpr *) fcinfo->flinfo->fn_expr;
		Node        *firstArg = linitial(funcExpr->args);

		if (IsA(firstArg, Var)) {
			Var           *var          = (Var *) firstArg;
			QueryDesc     *currentQuery = linitial(currentQueryStack);
			RangeTblEntry *rentry       = rt_fetch(var->varnoold, currentQuery->plannedstmt->rtable);

			PG_RETURN_FLOAT4(scoring_lookup_score(rentry->relid, ctid));
		} else {
			elog(ERROR, "zdb_score()'s argument is not a direct table ctid column reference");
		}
	}
}
