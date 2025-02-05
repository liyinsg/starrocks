// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

package com.starrocks.sql.ast;

import com.google.common.collect.ImmutableList;
import com.starrocks.analysis.Expr;
import com.starrocks.analysis.LimitElement;
import com.starrocks.analysis.OrderByElement;
import com.starrocks.analysis.RedirectStatus;
import com.starrocks.analysis.ShowStmt;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.ScalarType;
import com.starrocks.common.proc.ProcNodeInterface;
import com.starrocks.common.proc.RollupProcDir;
import com.starrocks.common.proc.SchemaChangeProcDir;
import com.starrocks.common.util.OrderByPair;
import com.starrocks.qe.ShowResultSetMetaData;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

/*
 * ShowAlterStmt: used to show process state of alter statement.
 * Syntax:
 *      SHOW ALTER TABLE [COLUMN | ROLLUP | MATERIALIZED_VIEW] [FROM dbName] [WHERE TableName="xxx"]
 *      [ORDER BY CreateTime DESC] [LIMIT [offset,]rows]
 */
public class ShowAlterStmt extends ShowStmt {

    public enum AlterType {
        COLUMN, ROLLUP, MATERIALIZED_VIEW
    }

    private final AlterType type;
    private String dbName;
    private final Expr whereClause;
    private HashMap<String, Expr> filterMap;
    private final List<OrderByElement> orderByElements;
    private ArrayList<OrderByPair> orderByPairs;
    private final LimitElement limitElement;
    private ProcNodeInterface node;

    public ShowAlterStmt(AlterType type, String dbName, Expr whereClause, List<OrderByElement> orderByElements,
                         LimitElement limitElement) {
        this.type = type;
        this.dbName = dbName;
        this.whereClause = whereClause;
        this.orderByElements = orderByElements;
        this.limitElement = limitElement;
    }

    public AlterType getType() {
        return type;
    }

    public String getDbName() {
        return dbName;
    }

    public void setDbName(String dbName) {
        this.dbName = dbName;
    }

    public HashMap<String, Expr> getFilterMap() {
        return filterMap;
    }

    public LimitElement getLimitElement() {
        return limitElement;
    }

    public ArrayList<OrderByPair> getOrderPairs() {
        return orderByPairs;
    }

    public ProcNodeInterface getNode() {
        return this.node;
    }

    public Expr getWhereClause() {
        return whereClause;
    }

    public List<OrderByElement> getOrderByElements() {
        return orderByElements;
    }

    public void setNode(ProcNodeInterface node) {
        this.node = node;
    }

    public void setFilter(HashMap<String, Expr> filterMap) {
        this.filterMap = filterMap;
    }

    public void setOrderByPairs(ArrayList<OrderByPair> orderByPairs) {
        this.orderByPairs = orderByPairs;
    }

    @Override
    public String toString() {
        return toSql();
    }

    @Override
    public ShowResultSetMetaData getMetaData() {
        ShowResultSetMetaData.Builder builder = ShowResultSetMetaData.builder();

        ImmutableList<String> titleNames = null;
        if (type == AlterType.ROLLUP || type == AlterType.MATERIALIZED_VIEW) {
            titleNames = RollupProcDir.TITLE_NAMES;
        } else if (type == AlterType.COLUMN) {
            titleNames = SchemaChangeProcDir.TITLE_NAMES;
        }

        for (String title : titleNames) {
            builder.addColumn(new Column(title, ScalarType.createVarchar(30)));
        }

        return builder.build();
    }

    @Override
    public <R, C> R accept(AstVisitor<R, C> visitor, C context) {
        return visitor.visitShowAlterStmt(this, context);
    }

    @Override
    public boolean isSupportNewPlanner() {
        return true;
    }

    @Override
    public RedirectStatus getRedirectStatus() {
        return RedirectStatus.FORWARD_NO_SYNC;
    }
}
