/*
 * qemu_interface.h: QEMU interface management
 *
 * Copyright (C) 2014, 2016 Red Hat, Inc.
 * Copyright IBM Corp. 2014
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

#include "qemu_conf.h"
#include "qemu_domain.h"
#include "qemu_slirp.h"

int qemuInterfaceDirectConnect(virDomainDef *def,
                               virQEMUDriver *driver,
                               virDomainNetDef *net,
                               int *tapfd,
                               size_t tapfdSize,
                               virNetDevVPortProfileOp vmop);

int qemuInterfaceOpenVhostNet(virDomainObj *def,
                              virDomainNetDef *net) ATTRIBUTE_MOCKABLE;

int qemuInterfacePrepareSlirp(virQEMUDriver *driver,
                              virDomainNetDef *net);
