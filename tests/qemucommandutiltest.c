/*
 * Copyright (C) 2015-2016 Red Hat, Inc.
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
 *
 */

#include <config.h>

#include "util/virjson.h"
#include "util/virqemu.h"
#include "testutils.h"

#define VIR_FROM_THIS VIR_FROM_NONE

typedef struct
{
    const char *props;
    const char *expectprops;
    virQEMUBuildCommandLineJSONArrayFormatFunc arrayfunc;
} testQemuCommandBuildObjectFromJSONData;

static int
testQemuCommandBuildFromJSON(const void *opaque)
{
    const testQemuCommandBuildObjectFromJSONData *data = opaque;
    g_autoptr(virJSONValue) val = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *result = NULL;

    if (!(val = virJSONValueFromString(data->props))) {
        fprintf(stderr, "Failed to parse JSON string '%s'", data->props);
        return -1;
    }

    if (virQEMUBuildCommandLineJSON(val, &buf, NULL, data->arrayfunc) < 0) {
        fprintf(stderr,
                "\nvirQEMUBuildCommandlineJSON failed process JSON:\n%s\n",
                data->props);
        return -1;
    }

    result = virBufferContentAndReset(&buf);

    if (STRNEQ_NULLABLE(data->expectprops, result)) {
        fprintf(stderr, "\nFailed to create object string. "
                "\nExpected:\n'%s'\nGot:\n'%s'",
                NULLSTR(data->expectprops), NULLSTR(result));
        return -1;
    }

    return 0;
}

static int
mymain(void)
{
    int ret = 0;
    testQemuCommandBuildObjectFromJSONData data1;

    virTestCounterReset("testQemuCommandBuildFromJSON");

#define DO_TEST_COMMAND_FROM_JSON(PROPS, ARRAYFUNC, EXPECT) \
    do { \
        data1.props = PROPS; \
        data1.expectprops = EXPECT; \
        data1.arrayfunc = ARRAYFUNC; \
        if (virTestRun(virTestCounterNext(), \
                       testQemuCommandBuildFromJSON, \
                       &data1) < 0) \
            ret = -1; \
     } while (0)

#define DO_TEST_COMMAND_DRIVE_FROM_JSON(PROPS, EXPECT) \
    DO_TEST_COMMAND_FROM_JSON(PROPS, virQEMUBuildCommandLineJSONArrayNumbered, EXPECT)

    DO_TEST_COMMAND_DRIVE_FROM_JSON("{\"driver\":\"gluster\","
                                     "\"volume\":\"test\","
                                     "\"path\":\"img\","
                                     "\"server\":[ { \"type\":\"tcp\","
                                                         "\"host\":\"example.com\","
                                                         "\"port\":\"1234\""
                                                        "},"
                                                        "{ \"type\":\"unix\","
                                                          "\"socket\":\"/path/socket\""
                                                        "},"
                                                        "{ \"type\":\"tcp\","
                                                          "\"host\":\"example.com\""
                                                        "}"
                                                       "]"
                                     "}",
                                     "driver=gluster,volume=test,path=img,"
                                     "server.0.type=tcp,"
                                     "server.0.host=example.com,"
                                     "server.0.port=1234,"
                                     "server.1.type=unix,"
                                     "server.1.socket=/path/socket,"
                                     "server.2.type=tcp,"
                                     "server.2.host=example.com");

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(mymain)
