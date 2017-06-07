/*
 * ======================================================================
 * Copyright (c) Microsoft Open Technologies, Inc.  All rights reserved.
 * Licensed under the MIT License.
 * See LICENSE.md in the project root for license information.
 * ======================================================================
*/

#include <QCoreApplication>
#include "LicenseParserTest.h"
#include "OfflinePublishTest.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    int res = 0;
    res += QTest::qExec(new LicenseParserTest(), argc, argv);
    res += QTest::qExec(new OfflinePublishTest(), argc, argv);

    return res;
}
