/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file implements functions for shaing lock information with the
** other processes.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#include "sqliteInt.h"
#include "btreeInt.h"
#include "rowlock_ipc.h"
#include "rowlock_ipc_table.h"
#include "rowlock_ipc_row.h"
#include "rowlock_os.h"

#define NEXT_IDX(n) ((n + pMeta->nElement + 1) % pMeta->nElement)
#define PREV_IDX(n) ((n + pMeta->nElement - 1) % pMeta->nElement)

IpcClass ipcClasses[] = {
  {rowClassMapName, rowClassIsInitialized, rowClassInitArea, rowClassElemCount, rowClassIsValid, rowClassElemIsTarget, rowClassElemGet, rowClassElemHash, rowClassElemClear, rowClassElemCopy, rowClassIndexPrev, rowClassIndexNext, rowClassCalcHash},
  {tableClassMapName, tableClassIsInitialized, tableClassInitArea, tableClassElemCount, tableClassIsValid, tableClassElemIsTarget, tableClassElemGet, tableClassElemHash, tableClassElemClear, tableClassElemCopy, tableClassIndexPrev, tableClassIndexNext, tableClassCalcHash},
};

/* For cleanup tool */
void sqlite3_rowlock_ipc_unlock_record_all(const char *name){
  sqlite3rowlockIpcUnlockRecordAll(name);
}

void sqlite3_rowlock_ipc_unlock_tables_all(const char *name){
  sqlite3rowlockIpcUnlockTablesAll(name);
}

/**********************************************************************/
/* For testing */
int sqlite3_rowlock_ipc_init(IpcHandle *pHandle, sqlite3_uint64 nByteRow, sqlite3_uint64 nByteTable, const void *owner, const char *name){
  return sqlite3rowlockIpcInit(pHandle, nByteRow, nByteTable, owner, name);
}

void sqlite3_rowlock_ipc_finish(IpcHandle *pHandle){
  sqlite3rowlockIpcFinish(pHandle);
}

int sqlite3_rowlock_ipc_lock_record(IpcHandle *pHandle, int iTable, sqlite3_int64 rowid){
  return sqlite3rowlockIpcLockRecord(pHandle, iTable, rowid);
}

void sqlite3_rowlock_ipc_unlock_record(IpcHandle *pHandle, int iTable, sqlite3_int64 rowid){
  sqlite3rowlockIpcUnlockRecord(pHandle, iTable, rowid);
}

void sqlite3_rowlock_ipc_unlock_record_proc(IpcHandle *pHandle, const char *name){
  sqlite3rowlockIpcUnlockRecordProc(pHandle, name);
}

int sqlite3_rowlock_ipc_lock_table(IpcHandle *pHandle, int iTable, unsigned char eLock, unsigned char *prevLock){
  return sqlite3rowlockIpcLockTable(pHandle, iTable, eLock, MODE_LOCK_NORMAL, prevLock);
}

unsigned char sqlite3_rowlock_ipc_lock_table_query(IpcHandle *pHandle, int iTable){
  return sqlite3rowlockIpcLockTableQuery(pHandle, iTable);
}

void sqlite3_rowlock_ipc_unlock_table(IpcHandle *pHandle, int iTable){
  sqlite3rowlockIpcUnlockTable(pHandle, iTable);
}

void sqlite3_rowlock_ipc_register_hash_func(int iClass, sqlite3_uint64(*xFunc)(void *pMap, ...)){
  if( xFunc ){
    ipcClasses[iClass].xCalcHash = xFunc;
  }else{
    u64(*hashFunc[])(void *pMap, ...) = {rowClassCalcHash, tableClassCalcHash};
    ipcClasses[iClass].xCalcHash = hashFunc[iClass];
  }
}
/**********************************************************************/

static int rowlockIpcMapName(u8 iClass, char *buf, size_t bufSize, const char *name){
  IpcClass *xClass = &ipcClasses[iClass];
  char fullPath[MAX_PATH_LEN] = {0};
  int rc;
  sqlite3_vfs *pVfs = sqlite3_vfs_find(0);

  rc = sqlite3OsFullPathname(pVfs, name, sizeof(fullPath), fullPath);
  if( rc ) return rc;
  rc = xClass->xMapName(buf, bufSize, fullPath);
  if( rc!=0 ) return SQLITE_CANTOPEN;
  return rc;
}

static int rowlockIpcCreate(u8 iClass, u64 allocSize, char *name, MMAP_HANDLE *phMap, void **ppMap){
  int rc = SQLITE_OK;
  MMAP_HANDLE hMap;
  void *pMap = NULL;

  IpcClass *xClass = &ipcClasses[iClass];

  rc = rowlockOsMmapOpen(allocSize, name, &hMap, &pMap);
  if( rc ) return rc;

  if( !xClass->xIsInitialized(pMap) ){
    xClass->xInitArea(pMap, allocSize);
  }

  *ppMap = pMap;
  *phMap = hMap;
  return SQLITE_OK;
}

static void rowlockIpcClose(MMAP_HANDLE hMap, void *pMap, MUTEX_HANDLE *pMutex){
/* Do not close mutex if it is saved in mmap (in case of Linux) */
#if SQLITE_OS_WIN
  rowlockOsMutexClose(pMutex);
#endif
  rowlockOsMmapClose(hMap, pMap);
}

/*
** Initialize a shared object used by multi processes.
** nByteRow:
**   The maximum memory allocation size in byte about mapped file for rowlock.
** nBytetable:
**   The maximum memory allocation size in byte about mapped file for tablelock and cachedRowid.
** owner:
**   It is used to identify the lock owner in the same process and thread.
**   pBtree is set to "owner" when sqlite3rowlockIpcLockRecord() is called by SQLite engine.
** name:
**  It is used for mapped filed name and mutext name with suffix. SQLite engine specifies DB full path. 
*/
int sqlite3rowlockIpcInit(IpcHandle *pHandle, u64 nByteRow, u64 nByteTable, const void *owner, const char *name){
  int rc;
  u64 nElemRow;
  u64 nElemTable;
  u64 nAllocRow;
  u64 nAllocTable;
  char mapNameRow[MAX_PATH_LEN] = {0};
  char mapNameTable[MAX_PATH_LEN] = {0};
  MMAP_HANDLE hRecordLock = {0};
  MMAP_HANDLE hTableLock = {0};
  void *pRecordLock = NULL;
  void *pTableLock = NULL;
  char mtxNameRow[MAX_PATH_LEN] = {0};
  char mtxNameTable[MAX_PATH_LEN] = {0};
  MUTEX_HANDLE rlMutex = {0};
  MUTEX_HANDLE tlMutex = {0};
#if SQLITE_OS_UNIX
  MUTEX_HANDLE *pMutex;
#endif
  /* Calculate MMAP sizes. */
  nElemRow = (nByteRow - sizeof(RowMetaData)) / sizeof(RowElement);
  nElemTable = (nByteTable - sizeof(TableMetaData)) / (sizeof(TableElement) + sizeof(CachedRowid));
  nAllocRow = sizeof(RowMetaData) + sizeof(RowElement) * nElemRow;
  nAllocTable = sizeof(TableMetaData) + (sizeof(TableElement) + sizeof(CachedRowid)) * nElemTable;

  /* Create MMAP and mutex names. */
  rc = rowlockIpcMapName(IPC_CLASS_ROW, mapNameRow, sizeof(mapNameRow), name);
  if( rc ) return rc;
  rc = rowlockIpcMapName(IPC_CLASS_TABLE, mapNameTable, sizeof(mapNameTable), name);
  if( rc ) return rc;
  rc = rowlockStrCat(mtxNameRow, sizeof(mtxNameRow), name, MUTEX_SUFFIX_ROWLOCK);
  if( rc ) return rc;
  rc = rowlockStrCat(mtxNameTable, sizeof(mtxNameTable), name, MUTEX_SUFFIX_TABLELOCK);
  if( rc ) return rc;

  /* Open MMAP. */
  rc = rowlockIpcCreate(IPC_CLASS_ROW, nAllocRow, mapNameRow, &hRecordLock, &pRecordLock);
  if( rc ) return rc;
  rc = rowlockIpcCreate(IPC_CLASS_TABLE, nAllocTable, mapNameTable, &hTableLock, &pTableLock);
  if( rc ) goto ipc_init_failed;

  /* Open mutex. */
#if SQLITE_OS_WIN
  rc = rowlockOsMutexOpen(mtxNameRow, &rlMutex);
  if( rc ) goto ipc_init_failed;
  rc = rowlockOsMutexOpen(mtxNameTable, &tlMutex);
  if( rc ) goto ipc_init_failed;
  memcpy(&pHandle->rlMutex, &rlMutex, sizeof(MUTEX_HANDLE));
  memcpy(&pHandle->tlMutex, &tlMutex, sizeof(MUTEX_HANDLE));
#else
  /* Do not open mutex if it is available in mmap */
  pMutex = &((RowMetaData*)pRecordLock)->mutex;
  if( pMutex->init!=1 ){
    rc = rowlockOsMutexOpen(mtxNameRow, &rlMutex);
    if( rc ) goto ipc_init_failed;
    memcpy(pMutex, &rlMutex, sizeof(MUTEX_HANDLE));
  }
  pMutex = &((TableMetaData*)pTableLock)->mutex;
  if( pMutex->init!=1 ){
    rc = rowlockOsMutexOpen(mtxNameTable, &tlMutex);
    if( rc ) goto ipc_init_failed;
    memcpy(pMutex, &tlMutex, sizeof(MUTEX_HANDLE));
  }
#endif

  /* Set output variable. */
  pHandle->pRecordLock = pRecordLock;
  pHandle->pTableLock = pTableLock; 
  pHandle->owner = (u64)owner;
  pHandle->hRecordLock = hRecordLock;
  pHandle->hTableLock = hTableLock;

  return SQLITE_OK;

ipc_init_failed:
  rowlockIpcClose(hRecordLock, pRecordLock, &rlMutex);
  rowlockIpcClose(hTableLock, pTableLock, &tlMutex);
  return rc;
}

void sqlite3rowlockIpcFinish(IpcHandle *pHandle){
  if( !pHandle ) return;
  sqlite3rowlockIpcUnlockRecordProc(pHandle, NULL);
  sqlite3rowlockIpcUnlockTablesProc(pHandle, NULL);
#if SQLITE_OS_WIN
  rowlockIpcClose(pHandle->hRecordLock, pHandle->pRecordLock, &pHandle->rlMutex);
  rowlockIpcClose(pHandle->hTableLock, pHandle->pTableLock, &pHandle->tlMutex);
#else
  rowlockIpcClose(pHandle->hRecordLock, pHandle->pRecordLock, &((RowMetaData*)pHandle->pRecordLock)->mutex);
  rowlockIpcClose(pHandle->hTableLock, pHandle->pTableLock, &((TableMetaData*)pHandle->pTableLock)->mutex);
#endif
  memset(pHandle, 0, sizeof(IpcHandle));
}

/* Calculate hash value. */
u64 rowlockIpcCalcHash(u64 nBucket, unsigned char *buf, u32 len){
  u64 h = 0;
  u32 i;

  /* The following code is refered by strHash() in hash.c. */
  for(i=0; i<len; i++){
    /* Knuth multiplicative hashing.  (Sorting & Searching, p. 510).
    ** 0x9e3779b1 is 2654435761 which is the closest prime number to
    ** (2**32)*golden_ratio, where golden_ratio = (sqrt(5) - 1)/2. */
    h += (unsigned char)buf[i];
    h *= 0x9e3779b1;
  }

  return h % nBucket;
}

/*
** Search row is locked or not.
** Return SQLITE_LOCKED if row is locked by myself or another user.
** Return SQLITE_OK if row is not locked. New entry should be store at *pIdx.
** Return SQLITE_NOMEM if row is not locked and it is impossible to store new entry.
*/
int rowlockIpcSearch(void *pMap, u8 iClass, void *pTarget, u64 hash, u64 *pIdx){
  int rc = SQLITE_OK;
  u64 idx = hash;
  IpcClass *xClass = &ipcClasses[iClass];
  void *pElem = xClass->xElemGet(pMap, idx);

  /* Search until element is not valid. */
  while( xClass->xElemIsValid(pElem) ){
    /* Check if the element is a target. */
    if( xClass->xElemIsTarget(pElem,pTarget) ){
      *pIdx = idx;
      return SQLITE_LOCKED;
    }
    idx = xClass->xIndexNext(pMap, idx);
    if( idx == hash ) {
      /* All entries are checked. */
      return SQLITE_NOMEM_BKPT;
    }
    pElem = xClass->xElemGet(pMap, idx);
  }

  *pIdx = idx;
  return SQLITE_OK;
}

static int isTargetPattern1and2(void *pMap, u8 iClass, u64 idxStart, u64 idxDel, u64 idx){
  IpcClass *xClass = &ipcClasses[iClass];
  u64 hash = xClass->xElemHash(pMap, idx);

  if( idxStart<=hash && hash<=idxDel ){
    return 1;
  }else{
    return 0;
  }
}

static int isTargetPattern3(void *pMap, u8 iClass, u64 idxStart, u64 idxDel, u64 idx){
  IpcClass *xClass = &ipcClasses[iClass];
  u64 hash = xClass->xElemHash(pMap, idx);

  if( hash<=idxDel || idxStart<=hash ){
    return 1;
  }else{
    return 0;
  }
}

/*
** Delete an element
*/
void rowlockIpcDelete(void *pMap, u8 iClass, u64 idxStart, u64 idxDel, u64 idxEnd){
  IpcClass *xClass = &ipcClasses[iClass];
  u64 idx;
  int (*xIsTarget)(void*,u8,u64,u64,u64);

  /* Search empty element. */
  idxStart = xClass->xIndexPrev(pMap,idxStart);
  while( xClass->xElemIsValid(xClass->xElemGet(pMap,idxStart)) ){
    idxStart = xClass->xIndexPrev(pMap, idxStart);
    if( idxStart == idxEnd ) {
      /* There is no empty element. */
      break;
    }
  }
  idxStart = xClass->xIndexNext(pMap, idxStart);
  /* Here, there is no empty element between idxStart and idxDel. */

  /* 
  ** Search a moving element.
  ** Range for the search is from idxEnd to idxDel.
  ** Acceptable element for the moving target is idxStart's hash <= hash <= idxDel's hash.
  ** There are 3 patterns.
  **
  ** pElem[0]                            pElem[N-1]
  ** 1. |--------|--------|--------|--------|
  **           Start     Del      End
  **                      <--------> Searching range
  **             <--------> Acceptable hash value
  **
  ** 2. |--------|--------|--------|--------|
  **            End     Start     Del
  **    <-------->                 <--------> Searching range
  **                      <--------> Acceptable hash value
  **
  ** 3. |--------|--------|--------|--------|
  **            Del      End     Start
  **             <--------> Searching range
  **    <-------->                 <--------> Acceptable hash value
  **
  */
  if( (idxStart<=idxDel && idxDel<=idxEnd) ||
      (idxEnd<=idxStart && idxStart<=idxDel) ){
    xIsTarget = isTargetPattern1and2;
  }else{
    assert( idxDel<=idxEnd && idxEnd<=idxStart );
    xIsTarget = isTargetPattern3;
  }

  for( idx=idxEnd; idx!=idxDel; idx=xClass->xIndexPrev(pMap,idx) ){
    if( xIsTarget(pMap, iClass, idxStart, idxDel, idx) ){
      break;
    }
  }
  /* Not found */
  if( idx == idxDel ) {
    xClass->xElemClear(pMap, idxDel);
  }else{
    xClass->xElemCopy(pMap, idxDel, idx);
    rowlockIpcDelete(pMap, iClass, idxStart, idx, idxEnd);
  }

  return;
}

#endif /* SQLITE_OMIT_ROWLOCK */
