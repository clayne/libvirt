/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_driver.c: Core Cloud-Hypervisor driver functions
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

#include <config.h>
#include <fcntl.h>

#include "ch_capabilities.h"
#include "ch_conf.h"
#include "ch_domain.h"
#include "ch_driver.h"
#include "ch_monitor.h"
#include "ch_process.h"
#include "domain_cgroup.h"
#include "domain_event.h"
#include "datatypes.h"
#include "driver.h"
#include "viraccessapicheck.h"
#include "virchrdev.h"
#include "virerror.h"
#include "virlog.h"
#include "virobject.h"
#include "virfile.h"
#include "virtypedparam.h"
#include "virutil.h"
#include "viruuid.h"
#include "virnuma.h"
#include "virhostmem.h"

#define VIR_FROM_THIS VIR_FROM_CH

VIR_LOG_INIT("ch.ch_driver");

virCHDriver *ch_driver = NULL;

/* Functions */
static int
chConnectURIProbe(char **uri)
{
    if (ch_driver == NULL)
        return 0;

    *uri = g_strdup("ch:///system");
    return 1;
}

static virDrvOpenStatus chConnectOpen(virConnectPtr conn,
                                      virConnectAuthPtr auth G_GNUC_UNUSED,
                                      virConf *conf G_GNUC_UNUSED,
                                      unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    /* URI was good, but driver isn't active */
    if (ch_driver == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Cloud-Hypervisor state driver is not active"));
        return VIR_DRV_OPEN_ERROR;
    }

    if (virConnectOpenEnsureACL(conn) < 0)
        return VIR_DRV_OPEN_ERROR;

    conn->privateData = ch_driver;

    return VIR_DRV_OPEN_SUCCESS;
}

static int chConnectClose(virConnectPtr conn)
{
    conn->privateData = NULL;
    return 0;
}

static const char *chConnectGetType(virConnectPtr conn)
{
    if (virConnectGetTypeEnsureACL(conn) < 0)
        return NULL;

    return "CH";
}

static int chConnectGetVersion(virConnectPtr conn,
                               unsigned long *version)
{
    virCHDriver *driver = conn->privateData;

    if (virConnectGetVersionEnsureACL(conn) < 0)
        return -1;

    *version = driver->version;
    return 0;
}

static char *chConnectGetHostname(virConnectPtr conn)
{
    if (virConnectGetHostnameEnsureACL(conn) < 0)
        return NULL;

    return virGetHostname();
}

static int chConnectNumOfDomains(virConnectPtr conn)
{
    virCHDriver *driver = conn->privateData;

    if (virConnectNumOfDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListNumOfDomains(driver->domains, true,
                                        virConnectNumOfDomainsCheckACL, conn);
}

static int chConnectListDomains(virConnectPtr conn, int *ids, int nids)
{
    virCHDriver *driver = conn->privateData;

    if (virConnectListDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListGetActiveIDs(driver->domains, ids, nids,
                                     virConnectListDomainsCheckACL, conn);
}

static int
chConnectListAllDomains(virConnectPtr conn,
                        virDomainPtr **domains,
                        unsigned int flags)
{
    virCHDriver *driver = conn->privateData;

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);

    if (virConnectListAllDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListExport(driver->domains, conn, domains,
                                 virConnectListAllDomainsCheckACL, flags);
}

static int chNodeGetInfo(virConnectPtr conn,
                         virNodeInfoPtr nodeinfo)
{
    if (virNodeGetInfoEnsureACL(conn) < 0)
        return -1;

    return virCapabilitiesGetNodeInfo(nodeinfo);
}

static char *chConnectGetCapabilities(virConnectPtr conn)
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virCaps) caps = NULL;
    char *xml;

    if (virConnectGetCapabilitiesEnsureACL(conn) < 0)
        return NULL;

    if (!(caps = virCHDriverGetCapabilities(driver, true)))
        return NULL;

    xml = virCapabilitiesFormatXML(caps);

    return xml;
}

static char *
chDomainManagedSavePath(virCHDriver *driver, virDomainObj *vm)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    return g_strdup_printf("%s/%s.save", cfg->saveDir, vm->def->name);
}


/**
 * chDomainCreateXML:
 * @conn: pointer to connection
 * @xml: XML definition of domain
 * @flags: bitwise-OR of supported virDomainCreateFlags
 *
 * Creates a domain based on xml and starts it
 *
 * Returns a new domain object or NULL in case of failure.
 */
static virDomainPtr
chDomainCreateXML(virConnectPtr conn,
                  const char *xml,
                  unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virDomainDef) vmdef = NULL;
    virDomainObj *vm = NULL;
    virDomainPtr dom = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;
    g_autofree char *managed_save_path = NULL;

    virCheckFlags(VIR_DOMAIN_START_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_START_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;


    if ((vmdef = virDomainDefParseString(xml, driver->xmlopt,
                                         NULL, parse_flags)) == NULL)
        goto cleanup;

    if (virDomainCreateXMLEnsureACL(conn, vmdef) < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(driver->domains,
                                   &vmdef,
                                   driver->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                       VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE,
                                   NULL)))
        goto cleanup;

    /* cleanup if there's any stale managedsave dir */
    managed_save_path = chDomainManagedSavePath(driver, vm);
    if (virFileDeleteTree(managed_save_path) < 0) {
        virReportSystemError(errno,
                             _("Failed to cleanup stale managed save dir '%1$s'"),
                             managed_save_path);
        goto cleanup;
    }

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virCHProcessStart(driver, vm, VIR_DOMAIN_RUNNING_BOOTED) < 0)
        goto endjob;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    if (vm && !dom) {
        virCHDomainRemoveInactive(driver, vm);
    }
    virDomainObjEndAPI(&vm);
    return dom;
}

static int
chDomainCreateWithFlags(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virCHDomainObjPrivate *priv;
    g_autofree char *managed_save_path = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainCreateWithFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (vm->hasManagedSave) {
        priv = vm->privateData;
        managed_save_path = chDomainManagedSavePath(driver, vm);
        if (virCHProcessStartRestore(driver, vm, managed_save_path) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to restore domain from managed save"));
            goto endjob;
        }
        if (virCHMonitorResumeVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to resume domain after restore from managed save"));
            goto endjob;
        }
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_RESTORED);
        /* cleanup the save dir after restore */
        if (virFileDeleteTree(managed_save_path) < 0) {
            virReportSystemError(errno,
                                 _("Failed to remove managed save path '%1$s'"),
                                 managed_save_path);
            goto endjob;
        }
        vm->hasManagedSave = false;
        ret = 0;
    } else {
        ret = virCHProcessStart(driver, vm, VIR_DOMAIN_RUNNING_BOOTED);
    }

    if (ret == 0) {
        virObjectEvent *event;

        event = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_STARTED,
                                                  VIR_DOMAIN_EVENT_STARTED_BOOTED);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainCreate(virDomainPtr dom)
{
    return chDomainCreateWithFlags(dom, 0);
}

static virDomainPtr
chDomainDefineXMLFlags(virConnectPtr conn, const char *xml, unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virDomainDef) vmdef = NULL;
    g_autoptr(virDomainDef) oldDef = NULL;
    virDomainObj *vm = NULL;
    virDomainPtr dom = NULL;
    virObjectEvent *event = NULL;
    g_autofree char *managed_save_path = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;

    virCheckFlags(VIR_DOMAIN_DEFINE_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_START_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;

    if ((vmdef = virDomainDefParseString(xml, driver->xmlopt,
                                         NULL, parse_flags)) == NULL)
        goto cleanup;

    if (virXMLCheckIllegalChars("name", vmdef->name, "\n") < 0)
        goto cleanup;

    if (virDomainDefineXMLFlagsEnsureACL(conn, vmdef) < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(driver->domains, &vmdef,
                                   driver->xmlopt,
                                   0, &oldDef)))
        goto cleanup;

    /* cleanup if there's any stale managedsave dir */
    managed_save_path = chDomainManagedSavePath(driver, vm);
    if (virFileDeleteTree(managed_save_path) < 0) {
        virReportSystemError(errno,
                             _("Failed to cleanup stale managed save dir '%1$s'"),
                             managed_save_path);
        goto cleanup;
    }

    vm->persistent = 1;
    event = virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_DEFINED,
                                              !oldDef ?
                                              VIR_DOMAIN_EVENT_DEFINED_ADDED :
                                              VIR_DOMAIN_EVENT_DEFINED_UPDATED);
    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);

    return dom;
}

static virDomainPtr
chDomainDefineXML(virConnectPtr conn, const char *xml)
{
    return chDomainDefineXMLFlags(conn, xml, 0);
}

static int
chDomainUndefineFlags(virDomainPtr dom,
                      unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virObjectEvent *event = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainUndefineFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("Cannot undefine transient domain"));
        goto cleanup;
    }
    event = virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_UNDEFINED,
                                              VIR_DOMAIN_EVENT_UNDEFINED_REMOVED);

    vm->persistent = 0;
    if (!virDomainObjIsActive(vm)) {
        virCHDomainRemoveInactive(driver, vm);
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);

    return ret;
}

static int
chDomainUndefine(virDomainPtr dom)
{
    return chDomainUndefineFlags(dom, 0);
}

static int chDomainIsActive(virDomainPtr dom)
{
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainIsActiveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = virDomainObjIsActive(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainShutdownFlags(virDomainPtr dom,
                      unsigned int flags)
{
    virCHDomainObjPrivate *priv;
    virDomainObj *vm;
    virDomainState state;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_SHUTDOWN_ACPI_POWER_BTN, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainShutdownFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    state = virDomainObjGetState(vm, NULL);
    if (state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("only can shutdown running/paused domain"));
        goto endjob;
    } else {
        if (virCHMonitorShutdownVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to shutdown guest VM"));
            goto endjob;
        }
    }

    virDomainObjSetState(vm, VIR_DOMAIN_SHUTDOWN, VIR_DOMAIN_SHUTDOWN_USER);

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainShutdown(virDomainPtr dom)
{
    return chDomainShutdownFlags(dom, 0);
}


static int
chDomainReboot(virDomainPtr dom, unsigned int flags)
{
    virCHDomainObjPrivate *priv;
    virDomainObj *vm;
    virDomainState state;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_REBOOT_ACPI_POWER_BTN, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainRebootEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    state = virDomainObjGetState(vm, NULL);
    if (state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("only can reboot running/paused domain"));
        goto endjob;
    } else {
        if (virCHMonitorRebootVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to reboot domain"));
            goto endjob;
        }
    }

    if (state == VIR_DOMAIN_RUNNING)
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_BOOTED);
    else
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_UNPAUSED);

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainSuspend(virDomainPtr dom)
{
    virCHDomainObjPrivate *priv;
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainSuspendEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_RUNNING) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("only can suspend running domain"));
        goto endjob;
    } else {
        if (virCHMonitorSuspendVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to suspend domain"));
            goto endjob;
        }
    }

    virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_USER);

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainResume(virDomainPtr dom)
{
    virCHDomainObjPrivate *priv;
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainResumeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("only can resume paused domain"));
        goto endjob;
    } else {
        if (virCHMonitorResumeVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to resume domain"));
            goto endjob;
        }
    }

    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_UNPAUSED);

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

/**
 * chDomainDestroyFlags:
 * @dom: pointer to domain to destroy
 * @flags: extra flags; not used yet.
 *
 * Sends SIGKILL to Cloud-Hypervisor process to terminate it
 *
 * Returns 0 on success or -1 in case of error
 */
static int
chDomainDestroyFlags(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virObjectEvent *event = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainDestroyFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_DESTROY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (virCHProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_DESTROYED) < 0)
        goto endjob;

    event = virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_STOPPED,
                                              VIR_DOMAIN_EVENT_STOPPED_DESTROYED);
    virCHDomainRemoveInactive(driver, vm);
    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);

    return ret;
}

static int
chDomainDestroy(virDomainPtr dom)
{
    return chDomainDestroyFlags(dom, 0);
}

static int
chDomainSaveRestoreAdditionalValidation(virCHDriver *driver,
                                        virDomainDef *vmdef)
{
    /* SAVE and RESTORE are functional only without any host device
     * passthrough configuration */
    if  (vmdef->nhostdevs > 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot save/restore domain with host devices"));
        return -1;
    }

    if (vmdef->nnets > 0) {
        if (!virBitmapIsBitSet(driver->chCaps, CH_RESTORE_WITH_NEW_TAPFDS)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cannot save/restore domain with network devices"));
            return -1;
        }
    }

    return 0;
}

/**
 * chDoDomainSave:
 * @driver: pointer to driver structure
 * @vm: pointer to virtual machine structure. Must be locked before invocation.
 * @to_dir: directory path (CH needs directory input) to save the domain
 * @managed: whether the VM is managed or not
 *
 * Checks if the domain is running or paused, then suspends it and saves it
 * using CH's vmm.snapshot API. CH creates multiple files for config, memory,
 * device state into @to_dir.
 *
 * Returns 0 on success or -1 in case of error
 */
static int
chDoDomainSave(virCHDriver *driver,
               virDomainObj *vm,
               const char *to_dir,
               bool managed)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    virCHDomainObjPrivate *priv = vm->privateData;
    CHSaveXMLHeader hdr;
    virDomainState domainState;
    g_autofree char *to = NULL;
    g_autofree char *xml = NULL;
    uint32_t xml_len;
    VIR_AUTOCLOSE fd = -1;
    int ret = -1;

    if (chDomainSaveRestoreAdditionalValidation(driver, vm->def) < 0)
        goto end;

    domainState = virDomainObjGetState(vm, NULL);
    if (domainState == VIR_DOMAIN_RUNNING) {
        if (virCHMonitorSuspendVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to suspend domain before saving"));
            goto end;
        }
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_SAVE);
    } else if (domainState != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("only can save running/paused domain"));
        goto end;
    }

    if (virDirCreate(to_dir, 0770, cfg->user, cfg->group,
                     VIR_DIR_CREATE_ALLOW_EXIST) < 0) {
        virReportSystemError(errno, _("Failed to create SAVE dir %1$s"), to_dir);
        goto end;
    }

    to = g_strdup_printf("%s/%s", to_dir, CH_SAVE_XML);
    if ((fd = virFileOpenAs(to, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR,
                            cfg->user, cfg->group, 0)) < 0) {
        virReportSystemError(-fd,
                             _("Failed to create/open domain save xml file '%1$s'"),
                             to);
        goto end;
    }

    if ((xml = virDomainDefFormat(vm->def, driver->xmlopt, 0)) == NULL)
        goto end;
    xml_len = strlen(xml) + 1;

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, CH_SAVE_MAGIC, sizeof(hdr.magic));
    hdr.xmlLen = xml_len;

    if (safewrite(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        virReportSystemError(errno, "%s", _("Failed to write file header"));
        goto end;
    }

    if (safewrite(fd, xml, xml_len) != xml_len) {
        virReportSystemError(errno, "%s", _("Failed to write xml definition"));
        goto end;
    }

    if (virCHMonitorSaveVM(priv->monitor, to_dir) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Failed to save domain"));
        goto end;
    }

    if (virCHProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_SAVED) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to shutoff after domain save"));
        goto end;
    }

    vm->hasManagedSave = managed;
    ret = 0;

 end:
    return ret;
}

static int
chDomainSaveFlags(virDomainPtr dom, const char *to, const char *dxml, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);
    if (dxml) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("xml modification unsupported"));
        return -1;
    }

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainSaveFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (chDoDomainSave(driver, vm, to, false) < 0)
        goto endjob;

    /* Remove if VM is not persistent */
    virCHDomainRemoveInactive(driver, vm);
    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainSave(virDomainPtr dom, const char *to)
{
    return chDomainSaveFlags(dom, to, NULL, 0);
}

static char *
chDomainSaveXMLRead(int fd)
{
    g_autofree char *xml = NULL;
    CHSaveXMLHeader hdr;

    if (saferead(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("failed to read CHSaveXMLHeader header"));
        return NULL;
    }

    if (memcmp(hdr.magic, CH_SAVE_MAGIC, sizeof(hdr.magic))) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("save image magic is incorrect"));
        return NULL;
    }

    if (hdr.xmlLen <= 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("invalid XML length: %1$d"), hdr.xmlLen);
        return NULL;
    }

    xml = g_new0(char, hdr.xmlLen);

    if (saferead(fd, xml, hdr.xmlLen) != hdr.xmlLen) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("failed to read XML"));
        return NULL;
    }

    return g_steal_pointer(&xml);
}

static int chDomainSaveImageRead(virCHDriver *driver,
                                 const char *path,
                                 virDomainDef **ret_def)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    g_autoptr(virDomainDef) def = NULL;
    g_autofree char *from = NULL;
    g_autofree char *xml = NULL;
    VIR_AUTOCLOSE fd = -1;
    int ret = -1;

    from = g_strdup_printf("%s/%s", path, CH_SAVE_XML);
    if ((fd = virFileOpenAs(from, O_RDONLY, 0, cfg->user, cfg->group, 0)) < 0) {
        virReportSystemError(errno,
                             _("Failed to open domain save file '%1$s'"),
                             from);
        goto end;
    }

    if (!(xml = chDomainSaveXMLRead(fd)))
        goto end;

    if (!(def = virDomainDefParseString(xml, driver->xmlopt, NULL,
                                        VIR_DOMAIN_DEF_PARSE_INACTIVE |
                                        VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE)))
        goto end;

    *ret_def = g_steal_pointer(&def);
    ret = 0;

 end:
    return ret;
}

static char *
chDomainSaveImageGetXMLDesc(virConnectPtr conn,
                            const char *path,
                            unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virDomainDef) def = NULL;
    char *ret = NULL;

    virCheckFlags(VIR_DOMAIN_SAVE_IMAGE_XML_SECURE, NULL);

    if (chDomainSaveImageRead(driver, path, &def) < 0)
        goto cleanup;

    if (virDomainSaveImageGetXMLDescEnsureACL(conn, def) < 0)
        goto cleanup;

    ret = virDomainDefFormat(def, driver->xmlopt,
                             virDomainDefFormatConvertXMLFlags(flags));

 cleanup:
    return ret;
}

static int
chDomainManagedSave(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm = NULL;
    g_autofree char *to = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainManagedSaveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot do managed save for transient domain"));
        goto endjob;
    }

    to = chDomainManagedSavePath(driver, vm);
    if (chDoDomainSave(driver, vm, to, true) < 0)
        goto endjob;

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainManagedSaveRemove(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    int ret = -1;
    g_autofree char *path = NULL;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainManagedSaveRemoveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    path = chDomainManagedSavePath(driver, vm);

    if (virFileDeleteTree(path) < 0) {
        virReportSystemError(errno,
                             _("Failed to remove managed save path '%1$s'"),
                             path);
        goto cleanup;
    }

    vm->hasManagedSave = false;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static char *
chDomainManagedSaveGetXMLDesc(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm = NULL;
    g_autoptr(virDomainDef) def = NULL;
    char *ret = NULL;
    g_autofree char *path = NULL;

    virCheckFlags(VIR_DOMAIN_SAVE_IMAGE_XML_SECURE, NULL);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    path = chDomainManagedSavePath(driver, vm);
    if (chDomainSaveImageRead(driver, path, &def) < 0)
        goto cleanup;

    if (virDomainManagedSaveGetXMLDescEnsureACL(dom->conn, def, flags) < 0)
        goto cleanup;

    ret = virDomainDefFormat(def, driver->xmlopt,
                             virDomainDefFormatConvertXMLFlags(flags));

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainHasManagedSaveImage(virDomainPtr dom, unsigned int flags)
{
    virDomainObj *vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainHasManagedSaveImageEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = vm->hasManagedSave;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainRestoreFlags(virConnectPtr conn,
                     const char *from,
                     const char *dxml,
                     unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    virDomainObj *vm = NULL;
    virCHDomainObjPrivate *priv;
    g_autoptr(virDomainDef) def = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (dxml) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("xml modification unsupported"));
        return -1;
    }

    if (chDomainSaveImageRead(driver, from, &def) < 0)
        goto cleanup;

    if (virDomainRestoreFlagsEnsureACL(conn, def) < 0)
        goto cleanup;

    if (chDomainSaveRestoreAdditionalValidation(driver, def) < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(driver->domains, &def,
                                   driver->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                   VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE,
                                   NULL)))
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virCHProcessStartRestore(driver, vm, from) < 0)
        goto endjob;

    priv = vm->privateData;
    if (virCHMonitorResumeVM(priv->monitor) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to resume domain after restore"));
        goto endjob;
    }
    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_RESTORED);
    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    if (vm && ret < 0)
        virCHDomainRemoveInactive(driver, vm);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainRestore(virConnectPtr conn, const char *from)
{
    return chDomainRestoreFlags(conn, from, NULL, 0);
}

static virDomainPtr chDomainLookupByID(virConnectPtr conn,
                                       int id)
{
    virCHDriver *driver = conn->privateData;
    virDomainObj *vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByID(driver->domains, id);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching id '%1$d'"), id);
        goto cleanup;
    }

    if (virDomainLookupByIDEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static virDomainPtr chDomainLookupByName(virConnectPtr conn,
                                         const char *name)
{
    virCHDriver *driver = conn->privateData;
    virDomainObj *vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByName(driver->domains, name);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%1$s'"), name);
        goto cleanup;
    }

    if (virDomainLookupByNameEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static virDomainPtr chDomainLookupByUUID(virConnectPtr conn,
                                         const unsigned char *uuid)
{
    virCHDriver *driver = conn->privateData;
    virDomainObj *vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByUUID(driver->domains, uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%1$s'"), uuidstr);
        goto cleanup;
    }

    if (virDomainLookupByUUIDEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static int
chDomainGetState(virDomainPtr dom,
                 int *state,
                 int *reason,
                 unsigned int flags)
{
    virDomainObj *vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetStateEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    *state = virDomainObjGetState(vm, reason);
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static char *chDomainGetXMLDesc(virDomainPtr dom,
                                unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    char *ret = NULL;

    virCheckFlags(VIR_DOMAIN_XML_COMMON_FLAGS, NULL);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetXMLDescEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    ret = virDomainDefFormat(vm->def, driver->xmlopt,
                             virDomainDefFormatConvertXMLFlags(flags));

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int chDomainGetInfo(virDomainPtr dom,
                           virDomainInfoPtr info)
{
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    info->state = virDomainObjGetState(vm, NULL);

    info->cpuTime = 0;

    info->maxMem = virDomainDefGetMemoryTotal(vm->def);
    info->memory = vm->def->mem.cur_balloon;
    info->nrVirtCpu = virDomainDefGetVcpus(vm->def);

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainOpenConsole(virDomainPtr dom,
                    const char *dev_name,
                    virStreamPtr st,
                    unsigned int flags)
{
     virDomainObj *vm = NULL;
     int ret = -1;
     size_t i;
     virDomainChrDef *chr = NULL;
     virCHDomainObjPrivate *priv;

     virCheckFlags(VIR_DOMAIN_CONSOLE_SAFE | VIR_DOMAIN_CONSOLE_FORCE, -1);

     if (!(vm = virCHDomainObjFromDomain(dom)))
          goto cleanup;

     if (virDomainOpenConsoleEnsureACL(dom->conn, vm->def) < 0)
          goto cleanup;

     if (virDomainObjCheckActive(vm) < 0)
          goto cleanup;

     priv = vm->privateData;

     if (dev_name) {
          for (i = 0; !chr && i < vm->def->nconsoles; i++) {
               if (vm->def->consoles[i]->info.alias &&
                   STREQ(dev_name, vm->def->consoles[i]->info.alias))
                    chr = vm->def->consoles[i];
          }
          for (i = 0; !chr && i < vm->def->nserials; i++) {
               if (STREQ(dev_name, vm->def->serials[i]->info.alias))
                    chr = vm->def->serials[i];
          }
     } else {
          if (vm->def->nconsoles &&
              vm->def->consoles[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)
               chr = vm->def->consoles[0];
          else if (vm->def->nserials &&
                   vm->def->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)
               chr = vm->def->serials[0];
     }

     if (!chr) {
          virReportError(VIR_ERR_INTERNAL_ERROR, _("cannot find character device %1$s"),
                         NULLSTR(dev_name));
          goto cleanup;
     }

     if (chr->source->type != VIR_DOMAIN_CHR_TYPE_PTY) {
          virReportError(VIR_ERR_INTERNAL_ERROR,
                         _("character device %1$s is not using a PTY"),
                         dev_name ? dev_name : NULLSTR(chr->info.alias));
          goto cleanup;
     }

     /* handle mutually exclusive access to console devices */
     ret = virChrdevOpen(priv->chrdevs, chr->source, st,
                         (flags & VIR_DOMAIN_CONSOLE_FORCE) != 0);

     if (ret == 1) {
          virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                         _("Active console session exists for this domain"));
          ret = -1;
     }

 cleanup:
     virDomainObjEndAPI(&vm);
     return ret;
}

static int chStateCleanup(void)
{
    if (ch_driver == NULL)
        return -1;

    virBitmapFree(ch_driver->chCaps);
    virObjectUnref(ch_driver->config);
    virObjectUnref(ch_driver->xmlopt);
    virObjectUnref(ch_driver->caps);
    virObjectUnref(ch_driver->domains);
    virObjectUnref(ch_driver->hostdevMgr);
    virObjectUnref(ch_driver->domainEventState);
    virMutexDestroy(&ch_driver->lock);
    g_clear_pointer(&ch_driver, g_free);

    return 0;
}

static virDrvStateInitResult
chStateInitialize(bool privileged,
                  const char *root,
                  bool monolithic G_GNUC_UNUSED,
                  virStateInhibitCallback callback G_GNUC_UNUSED,
                  void *opaque G_GNUC_UNUSED)
{
    int ret = VIR_DRV_STATE_INIT_ERROR;
    int rv;

    if (root != NULL) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Driver does not support embedded mode"));
        return -1;
    }

    ch_driver = g_new0(virCHDriver, 1);

    if (virMutexInit(&ch_driver->lock) < 0) {
        g_free(ch_driver);
        return VIR_DRV_STATE_INIT_ERROR;
    }

    if (!(ch_driver->domains = virDomainObjListNew()))
        goto cleanup;

    if (!(ch_driver->caps = virCHDriverCapsInit()))
        goto cleanup;

    if (!virCapabilitiesDomainSupported(ch_driver->caps, -1,
                                        VIR_ARCH_NONE, VIR_DOMAIN_VIRT_KVM, false) &&
        !virCapabilitiesDomainSupported(ch_driver->caps, -1,
                                        VIR_ARCH_NONE, VIR_DOMAIN_VIRT_HYPERV, false)) {
        VIR_INFO("/dev/kvm and /dev/mshv are missing. CH driver failed to initialize.");
        return VIR_DRV_STATE_INIT_SKIPPED;
    }

    if (!(ch_driver->xmlopt = chDomainXMLConfInit(ch_driver)))
        goto cleanup;

    if (!(ch_driver->config = virCHDriverConfigNew(privileged)))
        goto cleanup;

    if (!(ch_driver->hostdevMgr = virHostdevManagerGetDefault()))
        goto cleanup;

    if (!(ch_driver->domainEventState = virObjectEventStateNew()))
        goto cleanup;

    if ((rv = chExtractVersion(ch_driver)) < 0) {
        if (rv == -2)
            ret = VIR_DRV_STATE_INIT_SKIPPED;
        goto cleanup;
    }

    ch_driver->chCaps = virCHCapsInitCHVersionCaps(ch_driver->version);

    ch_driver->privileged = privileged;
    ret = VIR_DRV_STATE_INIT_COMPLETE;

 cleanup:
    if (ret != VIR_DRV_STATE_INIT_COMPLETE)
        chStateCleanup();
    return ret;
}

/* Which features are supported by this driver? */
static int
chConnectSupportsFeature(virConnectPtr conn,
                         int feature)
{
    int supported;

    if (virConnectSupportsFeatureEnsureACL(conn) < 0)
        return -1;

    if (virDriverFeatureIsGlobal(feature, &supported))
        return supported;

    switch ((virDrvFeature) feature) {
        case VIR_DRV_FEATURE_REMOTE:
        case VIR_DRV_FEATURE_PROGRAM_KEEPALIVE:
        case VIR_DRV_FEATURE_REMOTE_CLOSE_CALLBACK:
        case VIR_DRV_FEATURE_REMOTE_EVENT_CALLBACK:
        case VIR_DRV_FEATURE_TYPED_PARAM_STRING:
        case VIR_DRV_FEATURE_NETWORK_UPDATE_HAS_CORRECT_ORDER:
        case VIR_DRV_FEATURE_FD_PASSING:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Global feature %1$d should have already been handled"),
                           feature);
            return -1;
        case VIR_DRV_FEATURE_MIGRATION_V2:
        case VIR_DRV_FEATURE_MIGRATION_V3:
        case VIR_DRV_FEATURE_MIGRATION_P2P:
        case VIR_DRV_FEATURE_MIGRATE_CHANGE_PROTECTION:
        case VIR_DRV_FEATURE_XML_MIGRATABLE:
        case VIR_DRV_FEATURE_MIGRATION_OFFLINE:
        case VIR_DRV_FEATURE_MIGRATION_PARAMS:
        case VIR_DRV_FEATURE_MIGRATION_DIRECT:
        case VIR_DRV_FEATURE_MIGRATION_V1:
        default:
            return 0;
    }
}

static int
chDomainGetVcpusFlags(virDomainPtr dom,
                      unsigned int flags)
{
    virDomainObj *vm;
    virDomainDef *def;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM | VIR_DOMAIN_VCPU_GUEST, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainGetVcpusFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDef(vm, flags)))
        goto cleanup;

    if (flags & VIR_DOMAIN_VCPU_MAXIMUM)
        ret = virDomainDefGetVcpusMax(def);
    else
        ret = virDomainDefGetVcpus(def);


 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainGetMaxVcpus(virDomainPtr dom)
{
    return chDomainGetVcpusFlags(dom,
                                 (VIR_DOMAIN_AFFECT_LIVE |
                                  VIR_DOMAIN_VCPU_MAXIMUM));
}

static int
chDomainGetVcpuPinInfo(virDomain *dom,
                       int ncpumaps,
                       unsigned char *cpumaps,
                       int maplen,
                       unsigned int flags)
{
    virDomainObj *vm = NULL;
    virDomainDef *def;
    bool live;
    int ret = -1;

    g_autoptr(virBitmap) hostcpus = NULL;
    virBitmap *autoCpuset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetVcpuPinInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if (!(hostcpus = virHostCPUGetAvailableCPUsBitmap()))
        goto cleanup;

    if (live)
        autoCpuset = CH_DOMAIN_PRIVATE(vm)->autoCpuset;

    ret = virDomainDefGetVcpuPinInfoHelper(def, maplen, ncpumaps, cpumaps,
                                           hostcpus, autoCpuset);
 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chNodeGetCPUMap(virConnectPtr conn,
                unsigned char **cpumap,
                unsigned int *online, unsigned int flags)
{
    if (virNodeGetCPUMapEnsureACL(conn) < 0)
        return -1;

    return virHostCPUGetMap(cpumap, online, flags);
}


static int
chDomainHelperGetVcpus(virDomainObj *vm,
                       virVcpuInfoPtr info,
                       unsigned long long *cpuwait,
                       int maxinfo,
                       unsigned char *cpumaps,
                       int maplen)
{
    size_t ncpuinfo = 0;
    size_t i;

    if (maxinfo == 0)
        return 0;

    if (!virCHDomainHasVcpuPids(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cpu affinity is not supported"));
        return -1;
    }

    if (info)
        memset(info, 0, sizeof(*info) * maxinfo);

    if (cpumaps)
        memset(cpumaps, 0, sizeof(*cpumaps) * maxinfo);

    for (i = 0; i < virDomainDefGetVcpusMax(vm->def) && ncpuinfo < maxinfo; i++) {
        virDomainVcpuDef *vcpu = virDomainDefGetVcpu(vm->def, i);
        pid_t vcpupid = virCHDomainGetVcpuPid(vm, i);
        virVcpuInfoPtr vcpuinfo = info + ncpuinfo;

        if (!vcpu->online)
            continue;

        if (info) {
            vcpuinfo->number = i;
            vcpuinfo->state = VIR_VCPU_RUNNING;
            if (virProcessGetStatInfo(&vcpuinfo->cpuTime,
                                      NULL, NULL,
                                      &vcpuinfo->cpu, NULL,
                                      vm->pid, vcpupid) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("cannot get vCPU placement & pCPU time"));
                return -1;
            }
        }

        if (cpumaps) {
            unsigned char *cpumap = VIR_GET_CPUMAP(cpumaps, maplen, ncpuinfo);
            g_autoptr(virBitmap) map = NULL;

            if (!(map = virProcessGetAffinity(vcpupid)))
                return -1;

            virBitmapToDataBuf(map, cpumap, maplen);
        }

        if (cpuwait) {
            if (virProcessGetSchedInfo(&(cpuwait[ncpuinfo]), vm->pid, vcpupid) < 0)
                return -1;
        }

        ncpuinfo++;
    }

    return ncpuinfo;
}

static int
chDomainGetVcpus(virDomainPtr dom,
                 virVcpuInfoPtr info,
                 int maxinfo,
                 unsigned char *cpumaps,
                 int maplen)
{
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetVcpusEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot retrieve vcpu information for inactive domain"));
        goto cleanup;
    }

    ret = chDomainHelperGetVcpus(vm, info, NULL, maxinfo, cpumaps, maplen);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainPinVcpuLive(virDomainObj *vm,
                    virDomainDef *def,
                    int vcpu,
                    virCHDriver *driver,
                    virCHDriverConfig *cfg,
                    virBitmap *cpumap)
{
    g_autoptr(virBitmap) tmpmap = NULL;
    g_autoptr(virCgroup) cgroup_vcpu = NULL;
    virDomainVcpuDef *vcpuinfo;
    virCHDomainObjPrivate *priv = vm->privateData;

    if (!virCHDomainHasVcpuPids(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cpu affinity is not supported"));
        return -1;
    }

    if (!(vcpuinfo = virDomainDefGetVcpu(def, vcpu))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("vcpu %1$d is out of range of live cpu count %2$d"),
                       vcpu, virDomainDefGetVcpusMax(def));
        return -1;
    }

    if (!(tmpmap = virBitmapNewCopy(cpumap)))
        return -1;

    if (vcpuinfo->online) {
        /* Configure the corresponding cpuset cgroup before set affinity. */
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, vcpu,
                                   false, &cgroup_vcpu) < 0)
                return -1;
            if (virDomainCgroupSetupCpusetCpus(cgroup_vcpu, cpumap) < 0)
                return -1;
        }

        if (virProcessSetAffinity(virCHDomainGetVcpuPid(vm, vcpu), cpumap, false) < 0)
            return -1;
    }

    virBitmapFree(vcpuinfo->cpumask);
    vcpuinfo->cpumask = g_steal_pointer(&tmpmap);

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        return -1;

    return 0;
}

static int
chDomainPinVcpuFlags(virDomainPtr dom,
                     unsigned int vcpu,
                     unsigned char *cpumap,
                     int maplen,
                     unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virDomainDef *def;
    virDomainDef *persistentDef;
    int ret = -1;
    g_autoptr(virBitmap) pcpumap = NULL;
    virDomainVcpuDef *vcpuinfo = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virCHDriverGetConfig(driver);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainPinVcpuFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (persistentDef &&
        !(vcpuinfo = virDomainDefGetVcpu(persistentDef, vcpu))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("vcpu %1$d is out of range of persistent cpu count %2$d"),
                       vcpu, virDomainDefGetVcpus(persistentDef));
        goto endjob;
    }

    if (!(pcpumap = virBitmapNewData(cpumap, maplen)))
        goto endjob;

    if (virBitmapIsAllClear(pcpumap)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Empty cpu list for pinning"));
        goto endjob;
    }

    if (def &&
        chDomainPinVcpuLive(vm, def, vcpu, driver, cfg, pcpumap) < 0)
        goto endjob;

    if (persistentDef) {
        virBitmapFree(vcpuinfo->cpumask);
        vcpuinfo->cpumask = g_steal_pointer(&pcpumap);
        goto endjob;
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainPinVcpu(virDomainPtr dom,
                unsigned int vcpu,
                unsigned char *cpumap,
                int maplen)
{
    return chDomainPinVcpuFlags(dom, vcpu, cpumap, maplen,
                                VIR_DOMAIN_AFFECT_LIVE);
}



static int
chDomainGetEmulatorPinInfo(virDomainPtr dom,
                           unsigned char *cpumaps,
                           int maplen,
                           unsigned int flags)
{
    virDomainObj *vm = NULL;
    virDomainDef *def;
    virCHDomainObjPrivate *priv;
    bool live;
    int ret = -1;
    virBitmap *cpumask = NULL;
    g_autoptr(virBitmap) bitmap = NULL;
    virBitmap *autoCpuset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetEmulatorPinInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if (live) {
        priv = vm->privateData;
        autoCpuset = priv->autoCpuset;
    }
    if (def->cputune.emulatorpin) {
        cpumask = def->cputune.emulatorpin;
    } else if (def->cpumask) {
        cpumask = def->cpumask;
    } else if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO &&
               autoCpuset) {
        cpumask = autoCpuset;
    } else {
        if (!(bitmap = virHostCPUGetAvailableCPUsBitmap()))
            goto cleanup;
        cpumask = bitmap;
    }

    virBitmapToDataBuf(cpumask, cpumaps, maplen);

    ret = 1;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainPinEmulator(virDomainPtr dom,
                    unsigned char *cpumap,
                    int maplen,
                    unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virDomainDef *def;
    virDomainDef *persistentDef;
    int ret = -1;
    virCHDomainObjPrivate *priv;
    g_autoptr(virBitmap) pcpumap = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    virTypedParameterPtr eventParams = NULL;
    int eventNparams = 0;
    int eventMaxparams = 0;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virCHDriverGetConfig(driver);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainPinEmulatorEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    priv = vm->privateData;

    if (!(pcpumap = virBitmapNewData(cpumap, maplen)))
        goto endjob;

    if (virBitmapIsAllClear(pcpumap)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Empty cpu list for pinning"));
        goto endjob;
    }

    if (def) {
        g_autoptr(virCgroup) cgroup_emulator = NULL;
        g_autofree char *str = NULL;

        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_EMULATOR,
                                   0, false, &cgroup_emulator) < 0)
                goto endjob;

            if (virDomainCgroupSetupCpusetCpus(cgroup_emulator, pcpumap) < 0) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                               _("failed to set cpuset.cpus in cgroup for emulator threads"));
                goto endjob;
            }
        }

        if (virProcessSetAffinity(vm->pid, pcpumap, false) < 0)
            goto endjob;

        g_clear_pointer(&def->cputune.emulatorpin, virBitmapFree);

        if (!(def->cputune.emulatorpin = virBitmapNewCopy(pcpumap)))
            goto endjob;

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;

        str = virBitmapFormat(pcpumap);
        if (virTypedParamsAddString(&eventParams, &eventNparams,
                                    &eventMaxparams,
                                    VIR_DOMAIN_TUNABLE_CPU_EMULATORPIN,
                                    str) < 0)
            goto endjob;
    }

    if (persistentDef) {
        virBitmapFree(persistentDef->cputune.emulatorpin);
        persistentDef->cputune.emulatorpin = virBitmapNewCopy(pcpumap);

        /* Inactive XMLs are not saved, yet. */
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

#define CH_NB_NUMA_PARAM 2

static int
chDomainGetNumaParameters(virDomainPtr dom,
                          virTypedParameterPtr params,
                          int *nparams,
                          unsigned int flags)
{
    size_t i;
    virDomainObj *vm = NULL;
    virDomainNumatuneMemMode tmpmode = VIR_DOMAIN_NUMATUNE_MEM_STRICT;
    virCHDomainObjPrivate *priv;
    g_autofree char *nodeset = NULL;
    int ret = -1;
    virDomainDef *def = NULL;
    bool live = false;
    virBitmap *autoNodeset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;
    priv = vm->privateData;

    if (virDomainGetNumaParametersEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if (live)
        autoNodeset = priv->autoNodeset;

    if ((*nparams) == 0) {
        *nparams = CH_NB_NUMA_PARAM;
        ret = 0;
        goto cleanup;
    }

    for (i = 0; i < CH_NB_NUMA_PARAM && i < *nparams; i++) {
        virMemoryParameterPtr param = &params[i];

        switch (i) {
        case 0: /* fill numa mode here */
            ignore_value(virDomainNumatuneGetMode(def->numa, -1, &tmpmode));

            if (virTypedParameterAssign(param, VIR_DOMAIN_NUMA_MODE,
                                        VIR_TYPED_PARAM_INT, tmpmode) < 0)
                goto cleanup;

            break;

        case 1: /* fill numa nodeset here */
            nodeset = virDomainNumatuneFormatNodeset(def->numa, autoNodeset, -1);

            if (!nodeset ||
                virTypedParameterAssign(param, VIR_DOMAIN_NUMA_NODESET,
                                        VIR_TYPED_PARAM_STRING, nodeset) < 0)
                goto cleanup;

            nodeset = NULL;
            break;

        default:
            break;
            /* should not hit here */
        }
    }

    if (*nparams > CH_NB_NUMA_PARAM)
        *nparams = CH_NB_NUMA_PARAM;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainSetNumaParamsLive(virDomainObj *vm,
                          virBitmap *nodeset)
{
    g_autoptr(virCgroup) cgroup_temp = NULL;
    virCHDomainObjPrivate *priv = vm->privateData;
    g_autofree char *nodeset_str = NULL;
    virDomainNumatuneMemMode mode;
    size_t i = 0;

    if (virDomainNumatuneGetMode(vm->def->numa, -1, &mode) == 0 &&
        mode != VIR_DOMAIN_NUMATUNE_MEM_RESTRICTIVE) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("change of nodeset for running domain requires strict numa mode"));
        return -1;
    }

    if (!virNumaNodesetIsAvailable(nodeset))
        return -1;

    /* Ensure the cpuset string is formatted before passing to cgroup */
    nodeset_str = virBitmapFormat(nodeset);

    if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_EMULATOR, 0,
                           false, &cgroup_temp) < 0 ||
        virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
        return -1;


    for (i = 0; i < virDomainDefGetVcpusMax(vm->def); i++) {
        virDomainVcpuDef *vcpu = virDomainDefGetVcpu(vm->def, i);

        if (!vcpu->online)
            continue;

        if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, i,
                               false, &cgroup_temp) < 0 ||
            virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
            return -1;
    }

    for (i = 0; i < vm->def->niothreadids; i++) {
        if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_IOTHREAD,
                               vm->def->iothreadids[i]->iothread_id,
                               false, &cgroup_temp) < 0 ||
            virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
            return -1;
    }

    /* set nodeset for root cgroup */
    if (virCgroupSetCpusetMems(priv->cgroup, nodeset_str) < 0)
        return -1;

    return 0;
}

static int
chDomainSetNumaParameters(virDomainPtr dom,
                          virTypedParameterPtr params,
                          int nparams,
                          unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    size_t i;
    virDomainDef *def;
    virDomainDef *persistentDef;
    virDomainObj *vm = NULL;
    int ret = -1;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    virCHDomainObjPrivate *priv;
    g_autoptr(virBitmap) nodeset = NULL;
    virDomainNumatuneMemMode config_mode;
    int mode = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_NUMA_MODE,
                               VIR_TYPED_PARAM_INT,
                               VIR_DOMAIN_NUMA_NODESET,
                               VIR_TYPED_PARAM_STRING,
                               NULL) < 0)
        return -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainSetNumaParametersEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    priv = vm->privateData;
    cfg = virCHDriverGetConfig(driver);

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (STREQ(param->field, VIR_DOMAIN_NUMA_MODE)) {
            mode = param->value.i;

            if (mode < 0 || mode >= VIR_DOMAIN_NUMATUNE_MEM_LAST) {
                virReportError(VIR_ERR_INVALID_ARG,
                               _("unsupported numatune mode: '%1$d'"), mode);
                goto cleanup;
            }

        } else if (STREQ(param->field, VIR_DOMAIN_NUMA_NODESET)) {
            if (virBitmapParse(param->value.s, &nodeset,
                               VIR_DOMAIN_CPUMASK_LEN) < 0)
                goto cleanup;

            if (virBitmapIsAllClear(nodeset)) {
                virReportError(VIR_ERR_OPERATION_INVALID,
                               _("Invalid nodeset of 'numatune': %1$s"),
                               param->value.s);
                goto cleanup;
            }
        }
    }

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def) {
        if (!driver->privileged) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("NUMA tuning is not available in session mode"));
            goto endjob;
        }

        if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cgroup cpuset controller is not mounted"));
            goto endjob;
        }

        if (mode != -1 &&
            virDomainNumatuneGetMode(def->numa, -1, &config_mode) == 0 &&
            config_mode != mode) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("can't change numatune mode for running domain"));
            goto endjob;
        }

        if (nodeset &&
            chDomainSetNumaParamsLive(vm, nodeset) < 0)
            goto endjob;

        if (virDomainNumatuneSet(def->numa,
                                 def->placement_mode ==
                                 VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC,
                                 -1, mode, nodeset) < 0)
            goto endjob;

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }

    if (persistentDef) {
        if (virDomainNumatuneSet(persistentDef->numa,
                                 persistentDef->placement_mode ==
                                 VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC,
                                 -1, mode, nodeset) < 0)
            goto endjob;

        /* Inactive XMLs are not saved, yet. */
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chNodeGetMemoryStats(virConnectPtr conn,
                     int cellNum,
                     virNodeMemoryStatsPtr params,
                     int *nparams,
                     unsigned int flags)
{
    if (virNodeGetMemoryStatsEnsureACL(conn) < 0)
        return -1;

    return virHostMemGetStats(cellNum, params, nparams, flags);
}

static int
chConnectDomainEventRegisterAny(virConnectPtr conn,
                                virDomainPtr dom,
                                int eventID,
                                virConnectDomainEventGenericCallback callback,
                                void *opaque,
                                virFreeCallback freecb)
{
    virCHDriver *driver = conn->privateData;
    int ret = -1;

    if (virConnectDomainEventRegisterAnyEnsureACL(conn) < 0)
        return -1;

    if (virDomainEventStateRegisterID(conn,
                                      driver->domainEventState,
                                      dom, eventID,
                                      callback, opaque, freecb, &ret) < 0)
        ret = -1;

    return ret;
}


static int
chConnectDomainEventDeregisterAny(virConnectPtr conn,
                                  int callbackID)
{
    virCHDriver *driver = conn->privateData;

    if (virConnectDomainEventDeregisterAnyEnsureACL(conn) < 0)
        return -1;

    if (virObjectEventStateDeregisterID(conn,
                                        driver->domainEventState,
                                        callbackID, true) < 0)
        return -1;

    return 0;
}

static int
chDomainInterfaceAddresses(virDomain *dom,
                           virDomainInterfacePtr **ifaces,
                           unsigned int source,
                           unsigned int flags)
{
    virDomainObj *vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainInterfaceAddressesEnsureACL(dom->conn, vm->def, source) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    switch (source) {
    case VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE:
        ret = virDomainNetDHCPInterfaces(vm->def, ifaces);
        break;

    case VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP:
        ret = virDomainNetARPInterfaces(vm->def, ifaces);
        break;

    default:
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                       _("Unsupported IP address data source %1$d"),
                       source);
        break;
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


/* Function Tables */
static virHypervisorDriver chHypervisorDriver = {
    .name = "CH",
    .connectURIProbe = chConnectURIProbe,
    .connectOpen = chConnectOpen,                           /* 7.5.0 */
    .connectClose = chConnectClose,                         /* 7.5.0 */
    .connectGetType = chConnectGetType,                     /* 7.5.0 */
    .connectGetVersion = chConnectGetVersion,               /* 7.5.0 */
    .connectGetHostname = chConnectGetHostname,             /* 7.5.0 */
    .connectNumOfDomains = chConnectNumOfDomains,           /* 7.5.0 */
    .connectListAllDomains = chConnectListAllDomains,       /* 7.5.0 */
    .connectListDomains = chConnectListDomains,             /* 7.5.0 */
    .connectGetCapabilities = chConnectGetCapabilities,     /* 7.5.0 */
    .connectSupportsFeature = chConnectSupportsFeature,     /* 8.1.0 */
    .domainCreateXML = chDomainCreateXML,                   /* 7.5.0 */
    .domainCreate = chDomainCreate,                         /* 7.5.0 */
    .domainCreateWithFlags = chDomainCreateWithFlags,       /* 7.5.0 */
    .domainShutdown = chDomainShutdown,                     /* 7.5.0 */
    .domainShutdownFlags = chDomainShutdownFlags,           /* 7.5.0 */
    .domainReboot = chDomainReboot,                         /* 7.5.0 */
    .domainSuspend = chDomainSuspend,                       /* 7.5.0 */
    .domainResume = chDomainResume,                         /* 7.5.0 */
    .domainDestroy = chDomainDestroy,                       /* 7.5.0 */
    .domainDestroyFlags = chDomainDestroyFlags,             /* 7.5.0 */
    .domainDefineXML = chDomainDefineXML,                   /* 7.5.0 */
    .domainDefineXMLFlags = chDomainDefineXMLFlags,         /* 7.5.0 */
    .domainUndefine = chDomainUndefine,                     /* 7.5.0 */
    .domainUndefineFlags = chDomainUndefineFlags,           /* 7.5.0 */
    .domainLookupByID = chDomainLookupByID,                 /* 7.5.0 */
    .domainLookupByUUID = chDomainLookupByUUID,             /* 7.5.0 */
    .domainLookupByName = chDomainLookupByName,             /* 7.5.0 */
    .domainGetState = chDomainGetState,                     /* 7.5.0 */
    .domainGetXMLDesc = chDomainGetXMLDesc,                 /* 7.5.0 */
    .domainGetInfo = chDomainGetInfo,                       /* 7.5.0 */
    .domainIsActive = chDomainIsActive,                     /* 7.5.0 */
    .domainOpenConsole = chDomainOpenConsole,               /* 7.8.0 */
    .nodeGetInfo = chNodeGetInfo,                           /* 7.5.0 */
    .domainGetVcpus = chDomainGetVcpus,                     /* 8.0.0 */
    .domainGetVcpusFlags = chDomainGetVcpusFlags,           /* 8.0.0 */
    .domainGetMaxVcpus = chDomainGetMaxVcpus,               /* 8.0.0 */
    .domainGetVcpuPinInfo = chDomainGetVcpuPinInfo,         /* 8.0.0 */
    .domainPinVcpu = chDomainPinVcpu,                       /* 8.1.0 */
    .domainPinVcpuFlags = chDomainPinVcpuFlags,             /* 8.1.0 */
    .domainPinEmulator = chDomainPinEmulator,               /* 8.1.0 */
    .domainGetEmulatorPinInfo = chDomainGetEmulatorPinInfo, /* 8.1.0 */
    .nodeGetCPUMap = chNodeGetCPUMap,                       /* 8.0.0 */
    .domainSetNumaParameters = chDomainSetNumaParameters,   /* 8.1.0 */
    .domainGetNumaParameters = chDomainGetNumaParameters,   /* 8.1.0 */
    .domainSave = chDomainSave,                             /* 10.2.0 */
    .domainSaveFlags = chDomainSaveFlags,                   /* 10.2.0 */
    .domainSaveImageGetXMLDesc = chDomainSaveImageGetXMLDesc,   /* 10.2.0 */
    .domainManagedSave = chDomainManagedSave,               /* 10.2.0 */
    .domainManagedSaveRemove = chDomainManagedSaveRemove,   /* 10.2.0 */
    .domainManagedSaveGetXMLDesc = chDomainManagedSaveGetXMLDesc,   /* 10.2.0 */
    .domainHasManagedSaveImage = chDomainHasManagedSaveImage,   /* 10.2.0 */
    .domainRestore = chDomainRestore,                       /* 10.2.0 */
    .domainRestoreFlags = chDomainRestoreFlags,             /* 10.2.0 */
    .nodeGetMemoryStats = chNodeGetMemoryStats,             /* 10.10.0 */
    .connectDomainEventRegisterAny = chConnectDomainEventRegisterAny,       /* 10.10.0 */
    .connectDomainEventDeregisterAny = chConnectDomainEventDeregisterAny,   /* 10.10.0 */
    .domainInterfaceAddresses = chDomainInterfaceAddresses, /* 11.0.0 */
};

static virConnectDriver chConnectDriver = {
    .localOnly = true,
    .uriSchemes = (const char *[]){"ch", NULL},
    .hypervisorDriver = &chHypervisorDriver,
};

static virStateDriver chStateDriver = {
    .name = "cloud-hypervisor",
    .stateInitialize = chStateInitialize,
    .stateCleanup = chStateCleanup,
};

int chRegister(void)
{
    if (virRegisterConnectDriver(&chConnectDriver, true) < 0)
        return -1;
    if (virRegisterStateDriver(&chStateDriver) < 0)
        return -1;
    return 0;
}
