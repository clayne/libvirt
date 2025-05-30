/*
 * Copyright (C) 2013 Red Hat, Inc.
 * Copyright (C) 2012 Nicira, Inc.
 * Copyright (C) 2017 IBM Corporation
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
#include "virnetdevbandwidth.h"
#include "virnetdevvportprofile.h"
#include "virnetdevvlan.h"

#define VIR_NETDEV_OVS_DEFAULT_TIMEOUT 5

void virNetDevOpenvswitchSetTimeout(unsigned int timeout);

int virNetDevOpenvswitchAddPort(const char *brname,
                                const char *ifname,
                                const virMacAddr *macaddr,
                                const unsigned char *vmuuid,
                                const virNetDevVPortProfile *ovsport,
                                const virNetDevVlan *virtVlan)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3)
    G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchRemovePort(const char *ifname)
    ATTRIBUTE_NONNULL(1) G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchInterfaceGetMaster(const char *ifname, char **master)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchGetMigrateData(char **migrate, const char *ifname)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchSetMigrateData(char *migrate, const char *ifname)
    ATTRIBUTE_NONNULL(2) G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchInterfaceParseStats(const char *json,
                                            virDomainInterfaceStatsPtr stats)
    ATTRIBUTE_NONNULL(1) G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchInterfaceStats(const char *ifname,
                                       virDomainInterfaceStatsPtr stats)
    ATTRIBUTE_NONNULL(1) G_GNUC_WARN_UNUSED_RESULT;

int
virNetDevOpenvswitchMaybeUnescapeReply(char *reply)
    ATTRIBUTE_NONNULL(1) G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchGetVhostuserIfname(const char *path,
                                           bool server,
                                           char **ifname)
    ATTRIBUTE_NONNULL(3) G_GNUC_WARN_UNUSED_RESULT ATTRIBUTE_MOCKABLE;

int virNetDevOpenvswitchUpdateVlan(const char *ifname,
                                   const virNetDevVlan *virtVlan)
    ATTRIBUTE_NONNULL(1) G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchInterfaceSetQos(const char *ifname,
                                        const virNetDevBandwidth *bandwidth,
                                        const unsigned char *vmuuid,
                                        bool swapped)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(3) G_GNUC_WARN_UNUSED_RESULT;

int virNetDevOpenvswitchInterfaceClearQos(const char *ifname,
                                          const unsigned char *vmuuid)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) G_GNUC_WARN_UNUSED_RESULT;
