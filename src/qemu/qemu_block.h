/*
 * qemu_block.h: helper functions for QEMU block subsystem
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "internal.h"

#include "qemu_conf.h"
#include "qemu_domain.h"

#include "virjson.h"
#include "viruri.h"

void
qemuBlockStorageSourceSetStorageNodename(virStorageSource *src,
                                         char *nodename);

void
qemuBlockStorageSourceSetFormatNodename(virStorageSource *src,
                                        char *nodename);

const char *
qemuBlockStorageSourceGetEffectiveStorageNodename(virStorageSource *src);

const char *
qemuBlockStorageSourceGetStorageNodename(virStorageSource *src);

const char *
qemuBlockStorageSourceGetSliceNodename(virStorageSource *src);

const char *
qemuBlockStorageSourceGetFormatNodename(virStorageSource *src);

const char *
qemuBlockStorageSourceGetEffectiveNodename(virStorageSource *src);


typedef struct qemuBlockNodeNameBackingChainData qemuBlockNodeNameBackingChainData;
struct qemuBlockNodeNameBackingChainData {
    char *qemufilename; /* name of the image from qemu */
    char *nodeformat;   /* node name of the format layer */
    char *nodestorage;  /* node name of the storage backing the format node */

    qemuBlockNodeNameBackingChainData *backing;

    /* for testing purposes */
    char *drvformat;
    char *drvstorage;
};

bool
qemuBlockStorageSourceSupportsConcurrentAccess(virStorageSource *src);

typedef enum {
    QEMU_BLOCK_STORAGE_SOURCE_BACKEND_PROPS_LEGACY = 1 << 0,
    QEMU_BLOCK_STORAGE_SOURCE_BACKEND_PROPS_TARGET_ONLY = 1 << 1,
    QEMU_BLOCK_STORAGE_SOURCE_BACKEND_PROPS_EFFECTIVE_NODE = 1 << 2,
} qemuBlockStorageSourceBackendPropsFlags;

virJSONValue *
qemuBlockStorageSourceGetBackendProps(virStorageSource *src,
                                      unsigned int flags);

virURI *
qemuBlockStorageSourceGetURI(virStorageSource *src);

virJSONValue *
qemuBlockStorageSourceGetFormatProps(virStorageSource *src,
                                     virStorageSource *backingStore);

virJSONValue *
qemuBlockStorageGetCopyOnReadProps(virDomainDiskDef *disk);

typedef struct qemuBlockStorageSourceAttachData qemuBlockStorageSourceAttachData;
struct qemuBlockStorageSourceAttachData {
    virJSONValue *prmgrProps;
    char *prmgrAlias;

    virJSONValue *storageProps;
    const char *storageNodeName;
    bool storageAttached;

    virJSONValue *storageSliceProps;
    const char *storageSliceNodeName;
    bool storageSliceAttached;

    virJSONValue *formatProps;
    const char *formatNodeName;
    bool formatAttached;

    char *driveCmd;

    virQEMUCaps *qemuCaps; /* borrowed from VM, valid only for chardev-backed source */
    virDomainChrSourceDef *chardevDef;
    char *chardevAlias;
    bool chardevAdded;

    virJSONValue *authsecretProps;
    char *authsecretAlias;

    size_t encryptsecretCount;
    virJSONValue **encryptsecretProps;
    char **encryptsecretAlias;

    virJSONValue *httpcookiesecretProps;
    char *httpcookiesecretAlias;

    virJSONValue *tlsProps;
    char *tlsAlias;
    virJSONValue *tlsKeySecretProps;
    char *tlsKeySecretAlias;

    qemuFDPass *fdpass;
};


void
qemuBlockStorageSourceAttachDataFree(qemuBlockStorageSourceAttachData *data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(qemuBlockStorageSourceAttachData,
                        qemuBlockStorageSourceAttachDataFree);

qemuBlockStorageSourceAttachData *
qemuBlockStorageSourceAttachPrepareBlockdev(virStorageSource *src,
                                            virStorageSource *backingStore);

qemuBlockStorageSourceAttachData *
qemuBlockStorageSourceDetachPrepare(virStorageSource *src);

int
qemuBlockStorageSourceAttachApply(qemuMonitor *mon,
                                  qemuBlockStorageSourceAttachData *data);

void
qemuBlockStorageSourceAttachRollback(qemuMonitor *mon,
                                     qemuBlockStorageSourceAttachData *data);

struct _qemuBlockStorageSourceChainData {
    qemuBlockStorageSourceAttachData **srcdata;
    size_t nsrcdata;

    virJSONValue *copyOnReadProps;
    char *copyOnReadNodename;
    bool copyOnReadAttached;
};

typedef struct _qemuBlockStorageSourceChainData qemuBlockStorageSourceChainData;

void
qemuBlockStorageSourceChainDataFree(qemuBlockStorageSourceChainData *data);

qemuBlockStorageSourceChainData *
qemuBlockStorageSourceChainDetachPrepareBlockdev(virStorageSource *src);
qemuBlockStorageSourceChainData *
qemuBlockStorageSourceChainDetachPrepareChardev(char *chardevAlias);

int
qemuBlockStorageSourceChainAttach(qemuMonitor *mon,
                                  qemuBlockStorageSourceChainData *data);

void
qemuBlockStorageSourceChainDetach(qemuMonitor *mon,
                                  qemuBlockStorageSourceChainData *data);


G_DEFINE_AUTOPTR_CLEANUP_FUNC(qemuBlockStorageSourceChainData,
                        qemuBlockStorageSourceChainDataFree);

int
qemuBlockSnapshotAddBlockdev(virJSONValue *actions,
                             virDomainDiskDef *disk,
                             virStorageSource *newsrc);

char *
qemuBlockGetBackingStoreString(virStorageSource *src,
                               bool pretty)
    ATTRIBUTE_NONNULL(1);

int
qemuBlockStorageSourceCreateGetFormatProps(virStorageSource *src,
                                           virStorageSource *backing,
                                           virJSONValue **props)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(3) G_GNUC_WARN_UNUSED_RESULT;

int
qemuBlockStorageSourceCreateGetStorageProps(virStorageSource *src,
                                            virJSONValue **props)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) G_GNUC_WARN_UNUSED_RESULT;

int
qemuBlockStorageSourceCreate(virDomainObj *vm,
                             virStorageSource *src,
                             virStorageSource *backingStore,
                             virStorageSource *chain,
                             qemuBlockStorageSourceAttachData *data,
                             virDomainAsyncJob asyncJob);

int
qemuBlockStorageSourceCreateDetectSize(GHashTable *blockNamedNodeData,
                                       virStorageSource *src,
                                       virStorageSource *templ);

void
qemuBlockThrottleFilterSetNodename(virDomainThrottleFilterDef *filter,
                                   char *nodename);

const char *
qemuBlockThrottleFilterGetNodename(virDomainThrottleFilterDef *filter);

typedef struct qemuBlockThrottleFilterAttachData qemuBlockThrottleFilterAttachData;
struct qemuBlockThrottleFilterAttachData {
    virJSONValue *filterProps;
    const char *filterNodeName;
    bool filterAttached;
};

qemuBlockThrottleFilterAttachData *
qemuBlockThrottleFilterAttachPrepareBlockdev(virDomainThrottleFilterDef *throttlefilter,
                                             const char *parentNodeName);

void
qemuBlockThrottleFilterAttachDataFree(qemuBlockThrottleFilterAttachData *data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(qemuBlockThrottleFilterAttachData,
                              qemuBlockThrottleFilterAttachDataFree);

void
qemuBlockThrottleFilterAttachRollback(qemuMonitor *mon,
                                      qemuBlockThrottleFilterAttachData *data);

struct _qemuBlockThrottleFiltersData {
    qemuBlockThrottleFilterAttachData **filterdata;
    size_t nfilterdata;
};

typedef struct _qemuBlockThrottleFiltersData qemuBlockThrottleFiltersData;

void
qemuBlockThrottleFiltersDataFree(qemuBlockThrottleFiltersData *data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(qemuBlockThrottleFiltersData,
                              qemuBlockThrottleFiltersDataFree);

int
qemuBlockThrottleFiltersAttach(qemuMonitor *mon,
                               qemuBlockThrottleFiltersData *data);

void
qemuBlockThrottleFiltersDetach(qemuMonitor *mon,
                               qemuBlockThrottleFiltersData *data);

int
qemuBlockRemoveImageMetadata(virQEMUDriver *driver,
                             virDomainObj *vm,
                             const char *diskTarget,
                             virStorageSource *src);

qemuBlockNamedNodeDataBitmap *
qemuBlockNamedNodeDataGetBitmapByName(GHashTable *blockNamedNodeData,
                                      virStorageSource *src,
                                      const char *bitmap);

GHashTable *
qemuBlockGetNamedNodeData(virDomainObj *vm,
                          virDomainAsyncJob asyncJob);

int
qemuBlockGetBitmapMergeActions(virStorageSource *topsrc,
                               virStorageSource *basesrc,
                               virStorageSource *target,
                               const char *bitmapname,
                               const char *dstbitmapname,
                               virStorageSource *writebitmapsrc,
                               virJSONValue **actions,
                               GHashTable *blockNamedNodeData);

bool
qemuBlockBitmapChainIsValid(virStorageSource *src,
                            const char *bitmapname,
                            GHashTable *blockNamedNodeData);

int
qemuBlockBitmapsHandleBlockcopy(virStorageSource *src,
                                virStorageSource *mirror,
                                GHashTable *blockNamedNodeData,
                                bool shallow,
                                virJSONValue **actions);

int
qemuBlockBitmapsHandleCommitFinish(virStorageSource *topsrc,
                                   virStorageSource *basesrc,
                                   bool active,
                                   GHashTable *blockNamedNodeData,
                                   virJSONValue **actions);

int
qemuBlockReopenReadWrite(virDomainObj *vm,
                         virStorageSource *src,
                         virDomainAsyncJob asyncJob);
int
qemuBlockReopenReadOnly(virDomainObj *vm,
                        virStorageSource *src,
                        virDomainAsyncJob asyncJob);

int
qemuBlockReopenSliceExpand(virDomainObj *vm,
                           virStorageSource *src);

bool
qemuBlockStorageSourceIsLUKS(const virStorageSource *src);
bool
qemuBlockStorageSourceIsRaw(const virStorageSource *src);

bool
qemuBlockStorageSourceNeedsStorageSliceLayer(const virStorageSource *src);

bool
qemuBlockStorageSourceNeedsFormatLayer(const virStorageSource *src,
                                       virQEMUCaps *qemuCaps);

char *
qemuBlockStorageSourceGetCookieString(virStorageSource *src);

int
qemuBlockUpdateRelativeBacking(virDomainObj *vm,
                               virStorageSource *src,
                               virStorageSource *topsrc);

virJSONValue *
qemuBlockExportGetNBDProps(const char *nodename,
                           const char *exportname,
                           bool writable,
                           const char **bitmaps);


int
qemuBlockExportAddNBD(virDomainObj *vm,
                      virStorageSource *src,
                      const char *exportname,
                      bool writable,
                      const char *bitmap);

qemuBlockJobData *
qemuBlockCommit(virDomainObj *vm,
                virDomainDiskDef *disk,
                virStorageSource *baseSource,
                virStorageSource *topSource,
                virStorageSource *top_parent,
                unsigned long long bandwidth,
                virDomainAsyncJob asyncJob,
                virTristateBool autofinalize,
                unsigned int flags);

int
qemuBlockPivot(virDomainObj *vm,
               qemuBlockJobData *job,
               virDomainAsyncJob asyncJob,
               virDomainDiskDef *disk);

int
qemuBlockFinalize(virDomainObj *vm,
                  qemuBlockJobData *job,
                  virDomainAsyncJob asyncJob);
