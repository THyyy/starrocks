// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


package com.starrocks.sql.ast;

import com.google.common.collect.Lists;
import com.starrocks.analysis.LimitElement;
import com.starrocks.analysis.OrderByElement;
import com.starrocks.analysis.Predicate;
import com.starrocks.analysis.RedirectStatus;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.ScalarType;
import com.starrocks.catalog.Table;
import com.starrocks.common.MetaNotFoundException;
import com.starrocks.qe.ConnectContext;
import com.starrocks.qe.ShowResultSetMetaData;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.analyzer.Authorizer;
import com.starrocks.sql.analyzer.SemanticException;
import com.starrocks.sql.parser.NodePosition;
import com.starrocks.statistic.AnalyzeStatus;
import com.starrocks.statistic.StatisticUtils;
import com.starrocks.statistic.StatsConstants;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.time.format.DateTimeFormatter;
import java.util.List;

public class ShowAnalyzeStatusStmt extends ShowStmt {
    private static final Logger LOG = LogManager.getLogger(ShowAnalyzeStatusStmt.class);

    public ShowAnalyzeStatusStmt(Predicate predicate, List<OrderByElement> orderByElements,
                                 LimitElement limitElement, NodePosition pos) {
        super(pos);
        this.predicate = predicate;
        this.limitElement = limitElement;
        this.orderByElements = orderByElements;
    }

    public static final ShowResultSetMetaData META_DATA =
            ShowResultSetMetaData.builder()
                    .addColumn(new Column("Id", ScalarType.createVarchar(60)))
                    .addColumn(new Column("Database", ScalarType.createVarchar(60)))
                    .addColumn(new Column("Table", ScalarType.createVarchar(60)))
                    .addColumn(new Column("Columns", ScalarType.createVarchar(200)))
                    .addColumn(new Column("Type", ScalarType.createVarchar(20)))
                    .addColumn(new Column("Schedule", ScalarType.createVarchar(20)))
                    .addColumn(new Column("Status", ScalarType.createVarchar(20)))
                    .addColumn(new Column("StartTime", ScalarType.createVarchar(60)))
                    .addColumn(new Column("EndTime", ScalarType.createVarchar(60)))
                    .addColumn(new Column("Properties", ScalarType.createVarchar(200)))
                    .addColumn(new Column("Reason", ScalarType.createVarchar(100)))
                    .build();

    public static List<String> showAnalyzeStatus(ConnectContext context,
                                                 AnalyzeStatus analyzeStatus) throws MetaNotFoundException {
        List<String> row = Lists.newArrayList("", "", "", "ALL", "", "", "", "", "", "", "");
        List<String> columns = analyzeStatus.getColumns();

        row.set(0, String.valueOf(analyzeStatus.getId()));
        row.set(1, analyzeStatus.getCatalogName() + "." + analyzeStatus.getDbName());
        row.set(2, analyzeStatus.getTableName());

        Table table;
        // In new privilege framework(RBAC), user needs any action on the table to show analysis status for it.
        try {
            table = GlobalStateMgr.getCurrentState().getMetadataMgr().getTable(
                    context, analyzeStatus.getCatalogName(), analyzeStatus.getDbName(), analyzeStatus.getTableName());
            if (table == null) {
                throw new SemanticException("Table %s is not found", analyzeStatus.getTableName());
            }
            Authorizer.checkAnyActionOnTableLikeObject(context, analyzeStatus.getDbName(), table);
        } catch (Exception e) {
            LOG.warn("Failed to check privilege for show analyze status for table {}.", analyzeStatus.getTableName(), e);
            return null;
        }

        long totalCollectColumnsSize = StatisticUtils.getCollectibleColumns(table).size();
        if (null != columns && !columns.isEmpty() && (columns.size() != totalCollectColumnsSize)) {
            String str = String.join(",", columns);
            row.set(3, str);
        }

        row.set(4, analyzeStatus.getType().name());
        row.set(5, analyzeStatus.getScheduleType().name());
        if (analyzeStatus.getStatus().equals(StatsConstants.ScheduleStatus.FINISH)) {
            row.set(6, "SUCCESS");
        } else {
            String status = analyzeStatus.getStatus().name();
            if (analyzeStatus.getStatus().equals(StatsConstants.ScheduleStatus.RUNNING)) {
                status += " (" + analyzeStatus.getProgress() + "%" + ")";
            }
            row.set(6, status);
        }

        row.set(7, analyzeStatus.getStartTime().format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")));
        if (analyzeStatus.getEndTime() != null) {
            row.set(8, analyzeStatus.getEndTime().format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")));
        }

        row.set(9, analyzeStatus.getProperties() == null ? "{}" : analyzeStatus.getProperties().toString());

        if (analyzeStatus.getReason() != null) {
            row.set(10, analyzeStatus.getReason());
        }

        return row;
    }

    @Override
    public ShowResultSetMetaData getMetaData() {
        return META_DATA;
    }

    @Override
    public RedirectStatus getRedirectStatus() {
        return RedirectStatus.FORWARD_NO_SYNC;
    }

    @Override
    public <R, C> R accept(AstVisitor<R, C> visitor, C context) {
        return visitor.visitShowAnalyzeStatusStatement(this, context);
    }
}
