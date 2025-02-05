// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

package com.starrocks.sql.optimizer.rule.transformation;

import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.pattern.Pattern;
import com.starrocks.sql.optimizer.rule.RuleType;

import java.util.List;

public class InlineCTEConsumeRule extends TransformationRule {
    private static final InlineCTEConsumeRule instance = new InlineCTEConsumeRule();

    public static InlineCTEConsumeRule getInstance() {
        return instance;
    }

    private InlineCTEConsumeRule() {
        super(RuleType.TF_INLINE_CTE_CONSUME, Pattern.create(OperatorType.LOGICAL_CTE_CONSUME)
                .addChildren(Pattern.create(OperatorType.PATTERN_LEAF, OperatorType.PATTERN_MULTI_LEAF)));
    }

    @Override
    public List<OptExpression> transform(OptExpression input, OptimizerContext context) {
        // delete cte consume direct
        return input.getInputs();
    }
}
