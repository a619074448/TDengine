/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include "hash.h"
#include "taosdef.h"
#include "tchecksum.h"
#include "tsdb.h"
#include "tsdbMain.h"
#include "tskiplist.h"

#define TSDB_SUPER_TABLE_SL_LEVEL 5
#define DEFAULT_TAG_INDEX_COLUMN 0

static int     tsdbCompareSchemaVersion(const void *key1, const void *key2);
static int     tsdbRestoreTable(void *pHandle, void *cont, int contLen);
static void    tsdbOrgMeta(void *pHandle);
static char *  getTagIndexKey(const void *pData);
static STable *tsdbNewTable(STableCfg *pCfg, bool isSuper);
static void    tsdbFreeTable(STable *pTable);
static int     tsdbUpdateTableTagSchema(STable *pTable, STSchema *newSchema);
static int     tsdbAddTableToMeta(STsdbRepo *pRepo, STable *pTable, bool addIdx);
static void    tsdbRemoveTableFromMeta(STsdbRepo *pRepo, STable *pTable, bool rmFromIdx, bool lock);
static int     tsdbAddTableIntoIndex(STsdbMeta *pMeta, STable *pTable);
static int     tsdbRemoveTableFromIndex(STsdbMeta *pMeta, STable *pTable);
static int     tsdbInitTableCfg(STableCfg *config, ETableType type, uint64_t uid, int32_t tid);
static int     tsdbTableSetSchema(STableCfg *config, STSchema *pSchema, bool dup);
static int     tsdbTableSetName(STableCfg *config, char *name, bool dup);
static int     tsdbTableSetTagSchema(STableCfg *config, STSchema *pSchema, bool dup);
static int     tsdbTableSetSName(STableCfg *config, char *sname, bool dup);
static int     tsdbTableSetSuperUid(STableCfg *config, uint64_t uid);
static int     tsdbTableSetTagValue(STableCfg *config, SKVRow row, bool dup);
static int     tsdbTableSetStreamSql(STableCfg *config, char *sql, bool dup);
static int     tsdbEncodeTableName(void **buf, tstr *name);
static void *  tsdbDecodeTableName(void *buf, tstr **name);
static int     tsdbEncodeTable(void **buf, STable *pTable);
static void *  tsdbDecodeTable(void *buf, STable **pRTable);
static int     tsdbGetTableEncodeSize(int8_t act, STable *pTable);
static void *  tsdbInsertTableAct(STsdbRepo *pRepo, int8_t act, void *buf, STable *pTable);

// ------------------ OUTER FUNCTIONS ------------------
int tsdbCreateTable(TSDB_REPO_T *repo, STableCfg *pCfg) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  STsdbMeta *pMeta = pRepo->tsdbMeta;
  STable *   super = NULL;
  STable *   table = NULL;
  int        newSuper = 0;

  STable *pTable = tsdbGetTableByUid(pMeta, pCfg->tableId.uid);
  if (pTable != NULL) {
    tsdbError("vgId:%d table %s already exists, tid %d uid %" PRId64, REPO_ID(pRepo), TABLE_CHAR_NAME(pTable),
              TABLE_TID(pTable), TABLE_UID(pTable));
    return TSDB_CODE_TDB_TABLE_ALREADY_EXIST;
  }

  if (pCfg->type == TSDB_CHILD_TABLE) {
    super = tsdbGetTableByUid(pMeta, pCfg->superUid);
    if (super == NULL) {  // super table not exists, try to create it
      newSuper = 1;
      super = tsdbNewTable(pCfg, true);
      if (super == NULL) goto _err;
    } else {
      // TODO
      if (super->type != TSDB_SUPER_TABLE) return -1;
      if (super->tableId.uid != pCfg->superUid) return -1;
      tsdbUpdateTable(pRepo, super, pCfg);
    }
  }

  table = tsdbNewTable(pCfg, false);
  if (table == NULL) goto _err;

  // Register to meta
  if (newSuper) {
    if (tsdbAddTableToMeta(pRepo, super, true) < 0) goto _err;
  }
  if (tsdbAddTableToMeta(pRepo, table, true) < 0) goto _err;

  // Write to memtable action
  int   tlen1 = (newSuper) ? tsdbGetTableEncodeSize(TSDB_UPDATE_META, super) : 0;
  int   tlen2 = tsdbGetTableEncodeSize(TSDB_UPDATE_META, table);
  int   tlen = tlen1 + tlen2;
  void *buf = tsdbAllocBytes(pRepo, tlen);
  ASSERT(buf != NULL);
  if (newSuper) {
    void *pBuf = tsdbInsertTableAct(pRepo, TSDB_UPDATE_META, buf, super);
    ASSERT(POINTER_DISTANCE(pBuf, buf) == tlen1);
    buf = pBuf;
  }
  tsdbInsertTableAct(pRepo, TSDB_UPDATE_META, buf, table);

  return 0;

_err:
  tsdbFreeTable(super);
  tsdbFreeTable(table);
  return -1;
}

int tsdbDropTable(TSDB_REPO_T *repo, STableId tableId) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  STsdbMeta *pMeta = pRepo->tsdbMeta;
  uint64_t   uid = tableId.uid;
  int        tid = 0;
  char *     tbname = NULL;

  STable *pTable = tsdbGetTableByUid(pMeta, uid);
  if (pTable == NULL) {
    tsdbError("vgId:%d failed to drop table since table not exists! tid:%d uid %" PRId64, REPO_ID(pRepo), tableId.tid,
              uid);
    terrno = TSDB_CODE_TDB_INVALID_TABLE_ID;
    return -1;
  }

  tsdbTrace("vgId:%d try to drop table %s type %d", REPO_ID(pRepo), TABLE_CHAR_NAME(pTable), TABLE_TYPE(pTable));

  tid = TABLE_TID(pTable);
  tbname = strdup(TABLE_CHAR_NAME(pTable));
  if (tbname == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return -1;
  }

  if (TABLE_TYPE(pTable) == TSDB_STREAM_TABLE) {
    if (pTable->cqhandle) pRepo->appH.cqDropFunc(pTable->cqhandle);
  }

  if (TABLE_TYPE(pTable) == TSDB_SUPER_TABLE) {
    SSkipListIterator *pIter = tSkipListCreateIter(pTable->pIndex);
    while (tSkipListIterNext(pIter)) {
      STable *tTable = *(STable **)SL_GET_NODE_DATA(tSkipListIterGet(pIter));
      ASSERT(TABLE_TYPE(tTable) == TSDB_CHILD_TABLE);
      int   tlen = tsdbGetTableEncodeSize(TSDB_DROP_META, tTable);
      void *buf = tsdbAllocBytes(pRepo, tlen);
      ASSERT(buf != NULL);
      tsdbInsertTableAct(pRepo, TSDB_DROP_META, buf, tTable);
      tsdbRemoveTableFromMeta(pRepo, tTable, false, true);
    }
    tSkipListDestroyIter(pIter);
  }

  tsdbRemoveTableFromMeta(pRepo, pTable, true, true);

  tsdbTrace("vgId:%d, table %s is dropped! tid:%d, uid:%" PRId64, pRepo->config.tsdbId, tbname, tid, uid);
  free(tbname);

  return 0;
}

void *tsdbGetTableTagVal(const void* pTable, int32_t colId, int16_t type, int16_t bytes) {
  // TODO: this function should be changed also

  STSchema *pSchema = tsdbGetTableTagSchema((STable*) pTable);
  STColumn *pCol = tdGetColOfID(pSchema, colId);
  if (pCol == NULL) {
    return NULL;  // No matched tag volumn
  }

  char *val = tdGetKVRowValOfCol(((STable*)pTable)->tagVal, colId);
  assert(type == pCol->type && bytes == pCol->bytes);

  if (val != NULL && IS_VAR_DATA_TYPE(type)) {
    assert(varDataLen(val) < pCol->bytes);
  }

  return val;
}

char *tsdbGetTableName(void* pTable) {
  // TODO: need to change as thread-safe

  if (pTable == NULL) {
    return NULL;
  } else {
    return (char*) (((STable *)pTable)->name);
  }
}

STableId tsdbGetTableId(void *pTable) {
  assert(pTable);
  return ((STable*)pTable)->tableId;
}

STableCfg *tsdbCreateTableCfgFromMsg(SMDCreateTableMsg *pMsg) {
  if (pMsg == NULL) return NULL;

  SSchema *pSchema = (SSchema *)pMsg->data;
  int16_t  numOfCols = htons(pMsg->numOfColumns);
  int16_t  numOfTags = htons(pMsg->numOfTags);

  STSchemaBuilder schemaBuilder = {0};

  STableCfg *pCfg = (STableCfg *)calloc(1, sizeof(STableCfg));
  if (pCfg == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return NULL;
  }

  if (tsdbInitTableCfg(pCfg, pMsg->tableType, htobe64(pMsg->uid), htonl(pMsg->sid)) < 0) goto _err;
  if (tdInitTSchemaBuilder(&schemaBuilder, htonl(pMsg->sversion)) < 0) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  for (int i = 0; i < numOfCols; i++) {
    if (tdAddColToSchema(&schemaBuilder, pSchema[i].type, htons(pSchema[i].colId), htons(pSchema[i].bytes)) < 0) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      goto _err;
    }
  }
  if (tsdbTableSetSchema(pCfg, tdGetSchemaFromBuilder(&schemaBuilder), false) < 0) goto _err;
  if (tsdbTableSetName(pCfg, pMsg->tableId, true) < 0) goto _err;

  if (numOfTags > 0) {
    // Decode tag schema
    tdResetTSchemaBuilder(&schemaBuilder, htonl(pMsg->tversion));
    for (int i = numOfCols; i < numOfCols + numOfTags; i++) {
      if (tdAddColToSchema(&schemaBuilder, pSchema[i].type, htons(pSchema[i].colId), htons(pSchema[i].bytes)) < 0) {
        terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
        goto _err;
      }
    }
    if (tsdbTableSetTagSchema(pCfg, tdGetSchemaFromBuilder(&schemaBuilder), false) < 0) goto _err;
    if (tsdbTableSetSName(pCfg, pMsg->superTableId, true) < 0) goto _err;
    if (tsdbTableSetSuperUid(pCfg, htobe64(pMsg->superTableUid)) < 0) goto _err;

    // Decode tag values
    if (pMsg->tagDataLen) {
      int   accBytes = 0;
      char *pTagData = pMsg->data + (numOfCols + numOfTags) * sizeof(SSchema);

      SKVRowBuilder kvRowBuilder = {0};
      if (tdInitKVRowBuilder(&kvRowBuilder) < 0) {
        terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
        goto _err;
      }
      for (int i = numOfCols; i < numOfCols + numOfTags; i++) {
        if (tdAddColToKVRow(&kvRowBuilder, htons(pSchema[i].colId), pSchema[i].type, pTagData + accBytes) < 0) {
          terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
          goto _err;
        }
        accBytes += htons(pSchema[i].bytes);
      }

      tsdbTableSetTagValue(pCfg, tdGetKVRowFromBuilder(&kvRowBuilder), false);
      tdDestroyKVRowBuilder(&kvRowBuilder);
    }
  }

  if (pMsg->tableType == TSDB_STREAM_TABLE) {
    char *sql = pMsg->data + (numOfCols + numOfTags) * sizeof(SSchema);
    tsdbTableSetStreamSql(pCfg, sql, true);
  }

  tdDestroyTSchemaBuilder(&schemaBuilder);

  return pCfg;

_err:
  tdDestroyTSchemaBuilder(&schemaBuilder);
  tsdbClearTableCfg(pCfg);
  return NULL;
}

int tsdbUpdateTagValue(TSDB_REPO_T *repo, SUpdateTableTagValMsg *pMsg) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  STsdbMeta *pMeta = pRepo->tsdbMeta;
  int16_t    tversion = htons(pMsg->tversion);

  STable *pTable = tsdbGetTableByUid(pMeta, htobe64(pMsg->uid));
  if (pTable == NULL) {
    terrno = TSDB_CODE_TDB_INVALID_TABLE_ID;
    return -1;
  }
  if (TABLE_TID(pTable) != htonl(pMsg->tid)) {
    terrno = TSDB_CODE_TDB_INVALID_TABLE_ID;
    return -1;
  }

  if (TABLE_TYPE(pTable) != TSDB_CHILD_TABLE) {
    tsdbError("vgId:%d failed to update tag value of table %s since its type is %d", REPO_ID(pRepo),
              TABLE_CHAR_NAME(pTable), TABLE_TYPE(pTable));
    terrno = TSDB_CODE_TDB_INVALID_ACTION;
    return -1;
  }

  if (schemaVersion(tsdbGetTableTagSchema(pTable)) < tversion) {
    tsdbTrace("vgId:%d server tag version %d is older than client tag version %d, try to config", REPO_ID(pRepo),
              schemaVersion(tsdbGetTableTagSchema(pTable)), tversion);
    void *msg = (*pRepo->appH.configFunc)(pRepo->config.tsdbId, htonl(pMsg->tid));
    if (msg == NULL) return -1;

    // Deal with error her
    STableCfg *pTableCfg = tsdbCreateTableCfgFromMsg(msg);
    STable *   super = tsdbGetTableByUid(pMeta, pTableCfg->superUid);
    ASSERT(super != NULL);

    int32_t code = tsdbUpdateTable(pRepo, super, pTableCfg);
    if (code != TSDB_CODE_SUCCESS) {
      tsdbClearTableCfg(pTableCfg);
      return code;
    }
    tsdbClearTableCfg(pTableCfg);
    rpcFreeCont(msg);
  }

  STSchema *pTagSchema = tsdbGetTableTagSchema(pTable);

  if (schemaVersion(pTagSchema) > tversion) {
    tsdbError(
        "vgId:%d failed to update tag value of table %s since version out of date, client tag version %d server tag "
        "version %d",
        REPO_ID(pRepo), TABLE_CHAR_NAME(pTable), tversion, schemaVersion(pTable->tagSchema));
    return TSDB_CODE_TDB_TAG_VER_OUT_OF_DATE;
  }
  if (schemaColAt(pTagSchema, DEFAULT_TAG_INDEX_COLUMN)->colId == htons(pMsg->colId)) {
    tsdbRemoveTableFromIndex(pMeta, pTable);
  }
  // TODO: remove table from index if it is the first column of tag
  tdSetKVRowDataOfCol(&pTable->tagVal, htons(pMsg->colId), htons(pMsg->type), pMsg->data);
  if (schemaColAt(pTagSchema, DEFAULT_TAG_INDEX_COLUMN)->colId == htons(pMsg->colId)) {
    tsdbAddTableIntoIndex(pMeta, pTable);
  }
  return TSDB_CODE_SUCCESS;
}

// ------------------ INTERNAL FUNCTIONS ------------------
STsdbMeta *tsdbNewMeta(STsdbCfg *pCfg) {
  STsdbMeta *pMeta = (STsdbMeta *)calloc(1, sizeof(*pMeta));
  if (pMeta == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  int code = pthread_rwlock_init(&pMeta->rwLock, NULL);
  if (code != 0) {
    tsdbError("vgId:%d failed to init TSDB meta r/w lock since %s", pCfg->tsdbId, strerror(code));
    terrno = TAOS_SYSTEM_ERROR(code);
    goto _err;
  }

  pMeta->tables = (STable **)calloc(pCfg->maxTables, sizeof(STable *));
  if (pMeta->tables == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  pMeta->superList = tdListNew(sizeof(STable *));
  if (pMeta->superList == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  pMeta->uidMap = taosHashInit(pCfg->maxTables, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false);
  if (pMeta->uidMap == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  return pMeta;

_err:
  tsdbFreeMeta(pMeta);
  return NULL;
}

void tsdbFreeMeta(STsdbMeta *pMeta) {
  if (pMeta) {
    taosHashCleanup(pMeta->uidMap);
    tdListFree(pMeta->superList);
    tfree(pMeta->tables);
    pthread_rwlock_destroy(&pMeta->rwLock);
    free(pMeta);
  }
}

int tsdbOpenMeta(STsdbRepo *pRepo) {
  char *     fname = NULL;
  STsdbMeta *pMeta = pRepo->tsdbMeta;
  ASSERT(pMeta != NULL);

  fname = tsdbGetMetaFileName(pRepo->rootDir);
  if (fname == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  pMeta->pStore = tdOpenKVStore(fname, tsdbRestoreTable, tsdbOrgMeta, (void *)pRepo);
  if (pMeta->pStore == NULL) {
    tsdbError("vgId:%d failed to open TSDB meta while open the kv store since %s", REPO_ID(pRepo), tstrerror(terrno));
    goto _err;
  }

  tsdbTrace("vgId:%d open TSDB meta succeed", REPO_ID(pRepo));
  tfree(fname);
  return 0;

_err:
  tfree(fname);
  return -1;
}

int tsdbCloseMeta(STsdbRepo *pRepo) {
  STsdbCfg * pCfg = &pRepo->config;
  STsdbMeta *pMeta = pRepo->tsdbMeta;
  SListNode *pNode = NULL;
  STable *   pTable = NULL;

  if (pMeta == NULL) return 0;
  tdCloseKVStore(pMeta->pStore);
  for (int i = 1; i < pCfg->maxTables; i++) {
    tsdbFreeTable(pMeta->tables[i]);
  }

  while ((pNode = tdListPopHead(pMeta->superList)) != NULL) {
    tdListNodeGetData(pMeta->superList, pNode, (void *)(&pTable));
    tsdbFreeTable(pTable);
    listNodeFree(pNode);
  }

  tsdbTrace("vgId:%d TSDB meta is closed", REPO_ID(pRepo));
  return 0;
}

STSchema *tsdbGetTableSchema(STable *pTable) {
  if (pTable->type == TSDB_NORMAL_TABLE || pTable->type == TSDB_SUPER_TABLE || pTable->type == TSDB_STREAM_TABLE) {
    return pTable->schema[pTable->numOfSchemas - 1];
  } else if (pTable->type == TSDB_CHILD_TABLE) {
    STable *pSuper = pTable->pSuper;
    if (pSuper == NULL) return NULL;
    return pSuper->schema[pSuper->numOfSchemas - 1];
  } else {
    return NULL;
  }
}

STable *tsdbGetTableByUid(STsdbMeta *pMeta, uint64_t uid) {
  void *ptr = taosHashGet(pMeta->uidMap, (char *)(&uid), sizeof(uid));

  if (ptr == NULL) return NULL;

  return *(STable **)ptr;
}

STSchema *tsdbGetTableSchemaByVersion(STable *pTable, int16_t version) {
  STable *pSearchTable = (pTable->type == TSDB_CHILD_TABLE) ? pTable->pSuper : pTable;
  if (pSearchTable == NULL) return NULL;

  void *ptr = taosbsearch(&version, pSearchTable->schema, pSearchTable->numOfSchemas, sizeof(STSchema *),
                          tsdbCompareSchemaVersion, TD_EQ);
  if (ptr == NULL) return NULL;

  return *(STSchema **)ptr;
}

STSchema *tsdbGetTableTagSchema(STable *pTable) {
  if (pTable->type == TSDB_SUPER_TABLE) {
    return pTable->tagSchema;
  } else if (pTable->type == TSDB_CHILD_TABLE) {
    STable *pSuper = pTable->pSuper;
    if (pSuper == NULL) return NULL;
    return pSuper->tagSchema;
  } else {
    return NULL;
  }
}

int tsdbUpdateTable(STsdbRepo *pRepo, STable *pTable, STableCfg *pCfg) {
  // TODO: this function can only be called when there is no query and commit on this table
  ASSERT(TABLE_TYPE(pTable) != TSDB_CHILD_TABLE);
  bool       changed = false;
  STsdbMeta *pMeta = pRepo->tsdbMeta;

  if (pTable->type == TSDB_SUPER_TABLE) {
    if (schemaVersion(pTable->tagSchema) < schemaVersion(pCfg->tagSchema)) {
      if (tsdbUpdateTableTagSchema(pTable, pCfg->tagSchema) < 0) {
        tsdbError("vgId:%d failed to update table %s tag schema since %s", REPO_ID(pRepo), TABLE_CHAR_NAME(pTable),
                  tstrerror(terrno));
        return -1;
      }
    }
    changed = true;
  }

  STSchema *pTSchema = tsdbGetTableSchema(pTable);
  if (schemaVersion(pTSchema) < schemaVersion(pCfg->schema)) {
    if (pTable->numOfSchemas < TSDB_MAX_TABLE_SCHEMAS) {
      pTable->schema[pTable->numOfSchemas++] = tdDupSchema(pCfg->schema);
    } else {
      ASSERT(pTable->numOfSchemas == TSDB_MAX_TABLE_SCHEMAS);
      STSchema *tSchema = tdDupSchema(pCfg->schema);
      tdFreeSchema(pTable->schema[0]);
      memmove(pTable->schema, pTable->schema + 1, sizeof(STSchema *) * (TSDB_MAX_TABLE_SCHEMAS - 1));
      pTable->schema[pTable->numOfSchemas - 1] = tSchema;
    }

    pMeta->maxRowBytes = MAX(pMeta->maxRowBytes, dataRowMaxBytesFromSchema(pCfg->schema));
    pMeta->maxCols = MAX(pMeta->maxCols, schemaNCols(pCfg->schema));

    changed = true;
  }

  if (changed) {
    int   tlen = tsdbGetTableEncodeSize(TSDB_UPDATE_META, pTable);
    void *buf = tsdbAllocBytes(pRepo, tlen);
    tsdbInsertTableAct(pRepo, TSDB_UPDATE_META, buf, pTable);
  }

  return 0;
}

int tsdbWLockRepoMeta(STsdbRepo *pRepo) {
  int code = pthread_rwlock_wrlock(&(pRepo->tsdbMeta->rwLock));
  if (code != 0) {
    tsdbError("vgId:%d failed to write lock TSDB meta since %s", REPO_ID(pRepo), strerror(code));
    terrno = TAOS_SYSTEM_ERROR(code);
    return -1;
  }

  return 0;
}

int tsdbRLockRepoMeta(STsdbRepo *pRepo) {
  int code = pthread_rwlock_rdlock(&(pRepo->tsdbMeta->rwLock));
  if (code != 0) {
    tsdbError("vgId:%d failed to read lock TSDB meta since %s", REPO_ID(pRepo), strerror(code));
    terrno = TAOS_SYSTEM_ERROR(code);
    return -1;
  }

  return 0;
}

int tsdbUnlockRepoMeta(STsdbRepo *pRepo) {
  int code = pthread_rwlock_unlock(&(pRepo->tsdbMeta->rwLock));
  if (code != 0) {
    tsdbError("vgId:%d failed to unlock TSDB meta since %s", REPO_ID(pRepo), strerror(code));
    terrno = TAOS_SYSTEM_ERROR(code);
    return -1;
  }

  return 0;
}

void tsdbRefTable(STable *pTable) { T_REF_INC(pTable); }

void tsdbUnRefTable(STable *pTable) {
  if (T_REF_DEC(pTable) == 0) {
    if (TABLE_TYPE(pTable) == TSDB_CHILD_TABLE) {
      tsdbUnRefTable(pTable->pSuper);
    }
    tsdbFreeTable(pTable);
  }
}

// ------------------ LOCAL FUNCTIONS ------------------
static int tsdbCompareSchemaVersion(const void *key1, const void *key2) {
  if (*(int16_t *)key1 < schemaVersion(*(STSchema **)key2)) {
    return -1;
  } else if (*(int16_t *)key1 > schemaVersion(*(STSchema **)key2)) {
    return 1;
  } else {
    return 0;
  }
}

static int tsdbRestoreTable(void *pHandle, void *cont, int contLen) {
  STsdbRepo *pRepo = (STsdbRepo *)pHandle;
  STable *   pTable = NULL;

  if (!taosCheckChecksumWhole((uint8_t *)cont, contLen)) {
    terrno = TSDB_CODE_TDB_FILE_CORRUPTED;
    return -1;
  }

  tsdbDecodeTable(cont, &pTable);

  if (tsdbAddTableToMeta(pRepo, pTable, false) < 0) {
    tsdbFreeTable(pTable);
    return -1;
  }

  tsdbTrace("vgId:%d table %s tid %d uid %" PRIu64 " is restored from file", REPO_ID(pRepo), TABLE_CHAR_NAME(pTable),
            TABLE_TID(pTable), TABLE_UID(pTable));
  return 0;
}

static void tsdbOrgMeta(void *pHandle) {
  STsdbRepo *pRepo = (STsdbRepo *)pHandle;
  STsdbMeta *pMeta = pRepo->tsdbMeta;
  STsdbCfg * pCfg = &pRepo->config;

  for (int i = 1; i < pCfg->maxTables; i++) {
    STable *pTable = pMeta->tables[i];
    if (pTable != NULL && pTable->type == TSDB_CHILD_TABLE) {
      tsdbAddTableIntoIndex(pMeta, pTable);
    }
  }
}

static char *getTagIndexKey(const void *pData) {
  STable *pTable = *(STable **)pData;

  STSchema *pSchema = tsdbGetTableTagSchema(pTable);
  STColumn *pCol = schemaColAt(pSchema, DEFAULT_TAG_INDEX_COLUMN);
  void *    res = tdGetKVRowValOfCol(pTable->tagVal, pCol->colId);
  return res;
}

static STable *tsdbNewTable(STableCfg *pCfg, bool isSuper) {
  STable *pTable = NULL;
  size_t  tsize = 0;

  pTable = (STable *)calloc(1, sizeof(STable));
  if (pTable == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  if (isSuper) {
    pTable->type = TSDB_SUPER_TABLE;
    tsize = strnlen(pCfg->sname, TSDB_TABLE_NAME_LEN - 1);
    pTable->name = calloc(1, tsize + VARSTR_HEADER_SIZE + 1);
    if (pTable->name == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      goto _err;
    }
    STR_WITH_SIZE_TO_VARSTR(pTable->name, pCfg->sname, tsize);
    TABLE_UID(pTable) = pCfg->superUid;
    TABLE_TID(pTable) = -1;
    TABLE_SUID(pTable) = -1;
    pTable->pSuper = NULL;
    pTable->numOfSchemas = 1;
    pTable->schema[0] = tdDupSchema(pCfg->schema);
    if (pTable->schema[0] == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      goto _err;
    }
    pTable->tagSchema = tdDupSchema(pCfg->tagSchema);
    if (pTable->tagSchema == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      goto _err;
    }
    pTable->tagVal = NULL;
    STColumn *pCol = schemaColAt(pTable->tagSchema, DEFAULT_TAG_INDEX_COLUMN);
    pTable->pIndex = tSkipListCreate(TSDB_SUPER_TABLE_SL_LEVEL, colType(pCol), colBytes(pCol), 1, 0, 1, getTagIndexKey);
    if (pTable->pIndex == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      goto _err;
    }
  } else {
    pTable->type = pCfg->type;
    tsize = strnlen(pCfg->name, TSDB_TABLE_NAME_LEN - 1);
    pTable->name = calloc(1, tsize + VARSTR_HEADER_SIZE + 1);
    if (pTable->name == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      goto _err;
    }
    STR_WITH_SIZE_TO_VARSTR(pTable->name, pCfg->name, tsize);
    TABLE_UID(pTable) = pCfg->tableId.uid;
    TABLE_TID(pTable) = pCfg->tableId.tid;

    if (pCfg->type == TSDB_CHILD_TABLE) {
      TABLE_SUID(pTable) = pCfg->superUid;
      pTable->tagVal = tdKVRowDup(pCfg->tagValues);
      if (pTable->tagVal == NULL) {
        terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
        goto _err;
      }
    } else {
      TABLE_SUID(pTable) = -1;
      pTable->numOfSchemas = 1;
      pTable->schema[0] = tdDupSchema(pCfg->schema);
      if (pTable->schema[0] == NULL) {
        terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
        goto _err;
      }

      if (TABLE_TYPE(pTable) == TSDB_STREAM_TABLE) {
        pTable->sql = strdup(pCfg->sql);
        if (pTable->sql == NULL) {
          terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
          goto _err;
        }
      }
    }

    pTable->lastKey = TSKEY_INITIAL_VAL;
  }

  T_REF_INC(pTable);

  return pTable;

_err:
  tsdbFreeTable(pTable);
  return NULL;
}

static void tsdbFreeTable(STable *pTable) {
  if (pTable) {
    tsdbTrace("table %s is destroyed", TABLE_CHAR_NAME(pTable));
    tfree(TABLE_NAME(pTable));
    if (TABLE_TYPE(pTable) != TSDB_CHILD_TABLE) {
      for (int i = 0; i < TSDB_MAX_TABLE_SCHEMAS; i++) {
        tdFreeSchema(pTable->schema[i]);
      }

      if (TABLE_TYPE(pTable) == TSDB_SUPER_TABLE) {
        tdFreeSchema(pTable->tagSchema);
      }
    }

    kvRowFree(pTable->tagVal);

    tSkipListDestroy(pTable->pIndex);
    tfree(pTable->sql);
    free(pTable);
  }
}

static int tsdbUpdateTableTagSchema(STable *pTable, STSchema *newSchema) {
  ASSERT(pTable->type == TSDB_SUPER_TABLE);
  ASSERT(schemaVersion(pTable->tagSchema) < schemaVersion(newSchema));
  STSchema *pOldSchema = pTable->tagSchema;
  STSchema *pNewSchema = tdDupSchema(newSchema);
  if (pNewSchema == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return -1;
  }
  pTable->tagSchema = pNewSchema;
  tdFreeSchema(pOldSchema);

  return 0;
}

static int tsdbAddTableToMeta(STsdbRepo *pRepo, STable *pTable, bool addIdx) {
  STsdbMeta *pMeta = pRepo->tsdbMeta;

  if (addIdx && tsdbWLockRepoMeta(pRepo) < 0) {
    tsdbError("vgId:%d failed to add table %s to meta since %s", REPO_ID(pRepo), TABLE_CHAR_NAME(pTable),
              tstrerror(terrno));
    return -1;
  }

  if (TABLE_TYPE(pTable) == TSDB_SUPER_TABLE) {
    if (tdListAppend(pMeta->superList, (void *)(&pTable)) < 0) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      tsdbError("vgId:%d failed to add table %s to meta since %s", REPO_ID(pRepo), TABLE_CHAR_NAME(pTable),
                tstrerror(terrno));
      goto _err;
    }
  } else {
    if (TABLE_TYPE(pTable) == TSDB_CHILD_TABLE && addIdx) {  // add STABLE to the index
      if (tsdbAddTableIntoIndex(pMeta, pTable) < 0) {
        tsdbTrace("vgId:%d failed to add table %s to meta while add table to index since %s", REPO_ID(pRepo),
                  TABLE_CHAR_NAME(pTable), tstrerror(terrno));
        goto _err;
      }
    }
    pMeta->tables[TABLE_TID(pTable)] = pTable;
    pMeta->nTables++;
  }

  if (taosHashPut(pMeta->uidMap, (char *)(&pTable->tableId.uid), sizeof(pTable->tableId.uid), (void *)(&pTable),
                  sizeof(pTable)) < 0) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    tsdbError("vgId:%d failed to add table %s to meta while put into uid map since %s", REPO_ID(pRepo),
              TABLE_CHAR_NAME(pTable), tstrerror(terrno));
    goto _err;
  }

  if (TABLE_TYPE(pTable) != TSDB_CHILD_TABLE) {
    STSchema *pSchema = tsdbGetTableSchema(pTable);
    if (schemaNCols(pSchema) > pMeta->maxCols) pMeta->maxCols = schemaNCols(pSchema);
    if (schemaTLen(pSchema) > pMeta->maxRowBytes) pMeta->maxRowBytes = schemaTLen(pSchema);
  }

  if (addIdx && tsdbUnlockRepoMeta(pRepo) < 0) return -1;

  tsdbTrace("vgId:%d table %s tid %d uid %" PRIu64 " is added to meta", REPO_ID(pRepo), TABLE_CHAR_NAME(pTable),
            TABLE_TID(pTable), TABLE_UID(pTable));
  return 0;

_err:
  tsdbRemoveTableFromMeta(pRepo, pTable, false, false);
  if (addIdx) tsdbUnlockRepoMeta(pRepo);
  return -1;
}

static void tsdbRemoveTableFromMeta(STsdbRepo *pRepo, STable *pTable, bool rmFromIdx, bool lock) {
  STsdbMeta *pMeta = pRepo->tsdbMeta;
  SListIter  lIter = {0};
  SListNode *pNode = NULL;
  STable *   tTable = NULL;
  STsdbCfg * pCfg = &(pRepo->config);

  STSchema *pSchema = tsdbGetTableSchema(pTable);
  int       maxCols = schemaNCols(pSchema);
  int       maxRowBytes = schemaTLen(pSchema);

  if (lock) tsdbWLockRepoMeta(pRepo);

  if (TABLE_TYPE(pTable) == TSDB_SUPER_TABLE) {
    tdListInitIter(pMeta->superList, &lIter, TD_LIST_BACKWARD);

    while ((pNode = tdListNext(&lIter)) != NULL) {
      tdListNodeGetData(pMeta->superList, pNode, (void *)(&tTable));
      if (pTable == tTable) {
        tdListPopNode(pMeta->superList, pNode);
        free(pNode);
        break;
      }
    }
  } else {
    pMeta->tables[pTable->tableId.tid] = NULL;
    if (TABLE_TYPE(pTable) == TSDB_CHILD_TABLE && rmFromIdx) {
      tsdbRemoveTableFromIndex(pMeta, pTable);
    }

    pMeta->nTables--;
  }

  taosHashRemove(pMeta->uidMap, (char *)(&(TABLE_UID(pTable))), sizeof(TABLE_UID(pTable)));

  if (maxCols == pMeta->maxCols || maxRowBytes == pMeta->maxRowBytes) {
    maxCols = 0;
    maxRowBytes = 0;
    for (int i = 0; i < pCfg->maxTables; i++) {
      STable *pTable = pMeta->tables[i];
      if (pTable != NULL) {
        pSchema = tsdbGetTableSchema(pTable);
        maxCols = MAX(maxCols, schemaNCols(pSchema));
        maxRowBytes = MAX(maxRowBytes, schemaTLen(pSchema));
      }
    }
  }

  if (lock) tsdbUnlockRepoMeta(pRepo);
  tsdbTrace("vgId:%d table %s is removed from meta", REPO_ID(pRepo), TABLE_CHAR_NAME(pTable));
  tsdbUnRefTable(pTable);
}

static int tsdbAddTableIntoIndex(STsdbMeta *pMeta, STable *pTable) {
  ASSERT(pTable->type == TSDB_CHILD_TABLE && pTable != NULL);
  STable *pSTable = tsdbGetTableByUid(pMeta, TABLE_SUID(pTable));
  ASSERT(pSTable != NULL);

  pTable->pSuper = pSTable;

  int32_t level = 0;
  int32_t headSize = 0;

  tSkipListNewNodeInfo(pSTable->pIndex, &level, &headSize);

  // NOTE: do not allocate the space for key, since in each skip list node, only keep the pointer to pTable, not the
  // actual key value, and the key value will be retrieved during query through the pTable and getTagIndexKey function
  SSkipListNode *pNode = calloc(1, headSize + sizeof(STable *));
  if (pNode == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return -1;
  }
  pNode->level = level;

  memcpy(SL_GET_NODE_DATA(pNode), &pTable, sizeof(STable *));

  tSkipListPut(pSTable->pIndex, pNode);
  T_REF_INC(pSTable);
  return 0;
}

static int tsdbRemoveTableFromIndex(STsdbMeta *pMeta, STable *pTable) {
  ASSERT(pTable->type == TSDB_CHILD_TABLE && pTable != NULL);

  STable *pSTable = pTable->pSuper;
  ASSERT(pSTable != NULL);

  STSchema *pSchema = tsdbGetTableTagSchema(pTable);
  STColumn *pCol = schemaColAt(pSchema, DEFAULT_TAG_INDEX_COLUMN);

  char *  key = tdGetKVRowValOfCol(pTable->tagVal, pCol->colId);
  SArray *res = tSkipListGet(pSTable->pIndex, key);

  size_t size = taosArrayGetSize(res);
  ASSERT(size > 0);

  for (int32_t i = 0; i < size; ++i) {
    SSkipListNode *pNode = taosArrayGetP(res, i);

    // STableIndexElem* pElem = (STableIndexElem*) SL_GET_NODE_DATA(pNode);
    if (*(STable **)SL_GET_NODE_DATA(pNode) == pTable) {  // this is the exact what we need
      tSkipListRemoveNode(pSTable->pIndex, pNode);
    }
  }

  taosArrayDestroy(res);
  return 0;
}

static int tsdbInitTableCfg(STableCfg *config, ETableType type, uint64_t uid, int32_t tid) {
  if (type != TSDB_CHILD_TABLE && type != TSDB_NORMAL_TABLE && type != TSDB_STREAM_TABLE) {
    terrno = TSDB_CODE_TDB_INVALID_TABLE_TYPE;
    return -1;
  }

  memset((void *)config, 0, sizeof(*config));

  config->type = type;
  config->superUid = TSDB_INVALID_SUPER_TABLE_ID;
  config->tableId.uid = uid;
  config->tableId.tid = tid;
  return 0;
}

static int tsdbTableSetSchema(STableCfg *config, STSchema *pSchema, bool dup) {
  if (dup) {
    config->schema = tdDupSchema(pSchema);
    if (config->schema == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      return -1;
    }
  } else {
    config->schema = pSchema;
  }
  return 0;
}

static int tsdbTableSetName(STableCfg *config, char *name, bool dup) {
  if (dup) {
    config->name = strdup(name);
    if (config->name == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      return -1;
    }
  } else {
    config->name = name;
  }

  return 0;
}

static int tsdbTableSetTagSchema(STableCfg *config, STSchema *pSchema, bool dup) {
  if (config->type != TSDB_CHILD_TABLE) {
    terrno = TSDB_CODE_TDB_INVALID_CREATE_TB_MSG;
    return -1;
  }

  if (dup) {
    config->tagSchema = tdDupSchema(pSchema);
    if (config->tagSchema == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      return -1;
    }
  } else {
    config->tagSchema = pSchema;
  }
  return 0;
}

static int tsdbTableSetSName(STableCfg *config, char *sname, bool dup) {
  if (config->type != TSDB_CHILD_TABLE) {
    terrno = TSDB_CODE_TDB_INVALID_CREATE_TB_MSG;
    return -1;
  }

  if (dup) {
    config->sname = strdup(sname);
    if (config->sname == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      return -1;
    }
  } else {
    config->sname = sname;
  }
  return 0;
}

static int tsdbTableSetSuperUid(STableCfg *config, uint64_t uid) {
  if (config->type != TSDB_CHILD_TABLE || uid == TSDB_INVALID_SUPER_TABLE_ID) {
    terrno = TSDB_CODE_TDB_INVALID_CREATE_TB_MSG;
    return -1;
  }

  config->superUid = uid;
  return 0;
}

static int tsdbTableSetTagValue(STableCfg *config, SKVRow row, bool dup) {
  if (config->type != TSDB_CHILD_TABLE) {
    terrno = TSDB_CODE_TDB_INVALID_CREATE_TB_MSG;
    return -1;
  }

  if (dup) {
    config->tagValues = tdKVRowDup(row);
    if (config->tagValues == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      return -1;
    }
  } else {
    config->tagValues = row;
  }

  return 0;
}

static int tsdbTableSetStreamSql(STableCfg *config, char *sql, bool dup) {
  if (config->type != TSDB_STREAM_TABLE) {
    terrno = TSDB_CODE_TDB_INVALID_CREATE_TB_MSG;
    return -1;
  }

  if (dup) {
    config->sql = strdup(sql);
    if (config->sql == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      return -1;
    }
  } else {
    config->sql = sql;
  }

  return 0;
}

void tsdbClearTableCfg(STableCfg *config) {
  if (config) {
    if (config->schema) tdFreeSchema(config->schema);
    if (config->tagSchema) tdFreeSchema(config->tagSchema);
    if (config->tagValues) kvRowFree(config->tagValues);
    tfree(config->name);
    tfree(config->sname);
    tfree(config->sql);
    free(config);
  }
}

static int tsdbEncodeTableName(void **buf, tstr *name) {
  int tlen = 0;

  tlen += taosEncodeFixedI16(buf, name->len);
  if (buf != NULL) {
    memcpy(*buf, name->data, name->len);
    *buf = POINTER_SHIFT(*buf, name->len);
  }
  tlen += name->len;

  return tlen;
}

static void *tsdbDecodeTableName(void *buf, tstr **name) {
  VarDataLenT len = 0;

  buf = taosDecodeFixedI16(buf, &len);
  *name = calloc(1, sizeof(tstr) + len + 1);
  if (*name == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return NULL;
  }
  (*name)->len = len;
  memcpy((*name)->data, buf, len);

  buf = POINTER_SHIFT(buf, len);
  return buf;
}

static int tsdbEncodeTable(void **buf, STable *pTable) {
  ASSERT(pTable != NULL);
  int tlen = 0;

  tlen += taosEncodeFixedU8(buf, pTable->type);
  tlen += tsdbEncodeTableName(buf, pTable->name);
  tlen += taosEncodeFixedU64(buf, TABLE_UID(pTable));
  tlen += taosEncodeFixedI32(buf, TABLE_TID(pTable));

  if (TABLE_TYPE(pTable) == TSDB_CHILD_TABLE) {
    tlen += taosEncodeFixedU64(buf, TABLE_SUID(pTable));
    tlen += tdEncodeKVRow(buf, pTable->tagVal);
  } else {
    tlen += taosEncodeFixedU8(buf, pTable->numOfSchemas);
    for (int i = 0; i < pTable->numOfSchemas; i++) {
      tlen += tdEncodeSchema(buf, pTable->schema[i]);
    }

    if (TABLE_TYPE(pTable) == TSDB_SUPER_TABLE) {
      tlen += tdEncodeSchema(buf, pTable->tagSchema);
    }

    if (TABLE_TYPE(pTable) == TSDB_STREAM_TABLE) {
      tlen += taosEncodeString(buf, pTable->sql);
    }
  }

  return tlen;
}

static void *tsdbDecodeTable(void *buf, STable **pRTable) {
  STable *pTable = (STable *)calloc(1, sizeof(STable));
  if (pTable == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return NULL;
  }
  uint8_t type = 0;

  buf = taosDecodeFixedU8(buf, &type);
  pTable->type = type;
  buf = tsdbDecodeTableName(buf, &(pTable->name));
  buf = taosDecodeFixedU64(buf, &TABLE_UID(pTable));
  buf = taosDecodeFixedI32(buf, &TABLE_TID(pTable));

  if (TABLE_TYPE(pTable) == TSDB_CHILD_TABLE) {
    buf = taosDecodeFixedU64(buf, &TABLE_SUID(pTable));
    buf = tdDecodeKVRow(buf, &(pTable->tagVal));
  } else {
    buf = taosDecodeFixedU8(buf, &(pTable->numOfSchemas));
    for (int i = 0; i < pTable->numOfSchemas; i++) {
      buf = tdDecodeSchema(buf, &(pTable->schema[i]));
    }

    if (TABLE_TYPE(pTable) == TSDB_SUPER_TABLE) {
      buf = tdDecodeSchema(buf, &(pTable->tagSchema));
      STColumn *pCol = schemaColAt(pTable->tagSchema, DEFAULT_TAG_INDEX_COLUMN);
      pTable->pIndex =
          tSkipListCreate(TSDB_SUPER_TABLE_SL_LEVEL, colType(pCol), colBytes(pCol), 1, 0, 1, getTagIndexKey);
      if (pTable->pIndex == NULL) {
        terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
        tsdbFreeTable(pTable);
        return NULL;
      }
    }

    if (TABLE_TYPE(pTable) == TSDB_STREAM_TABLE) {
      buf = taosDecodeString(buf, &(pTable->sql));
    }
  }

  T_REF_INC(pTable);

  *pRTable = pTable;

  return buf;
}

static int tsdbGetTableEncodeSize(int8_t act, STable *pTable) {
  int tlen = sizeof(SListNode) + sizeof(SActObj);
  if (act == TSDB_UPDATE_META) tlen += (sizeof(SActCont) + tsdbEncodeTable(NULL, pTable) + sizeof(TSCKSUM));

  return tlen;
}

static void *tsdbInsertTableAct(STsdbRepo *pRepo, int8_t act, void *buf, STable *pTable) {
  SListNode *pNode = (SListNode *)buf;
  SActObj *  pAct = (SActObj *)(pNode->data);
  SActCont * pCont = (SActCont *)POINTER_SHIFT(pAct, sizeof(*pAct));
  void *     pBuf = (void *)pCont;

  pNode->prev = pNode->next = NULL;
  pAct->act = act;
  pAct->uid = TABLE_UID(pTable);

  if (act == TSDB_UPDATE_META) {
    pBuf = (void *)(pCont->cont);
    pCont->len = tsdbEncodeTable(&pBuf, pTable) + sizeof(TSCKSUM);
    taosCalcChecksumAppend(0, (uint8_t *)pCont->cont, pCont->len);
    pBuf = POINTER_SHIFT(pBuf, sizeof(TSCKSUM));
  }

  tdListAppendNode(pRepo->mem->actList, pNode);

  return pBuf;
}