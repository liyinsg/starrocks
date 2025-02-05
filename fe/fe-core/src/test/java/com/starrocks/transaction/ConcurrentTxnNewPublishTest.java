// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

package com.starrocks.transaction;

import com.starrocks.common.Config;

import java.sql.SQLException;

public class ConcurrentTxnNewPublishTest extends ConcurrentTxnTest {
    @Override
    void setup() throws SQLException {
        runTime = 20;
        withRead = true;
        Config.enable_new_publish_mechanism = false;
    }
}
