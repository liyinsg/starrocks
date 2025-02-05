// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/pipeline/crossjoin/nljoin_probe_operator.h"

#include "column/chunk.h"
#include "column/column_helper.h"
#include "exec/pipeline/crossjoin/cross_join_right_sink_operator.h"
#include "gen_cpp/PlanNodes_types.h"
#include "runtime/current_thread.h"
#include "runtime/descriptors.h"
#include "simd/simd.h"
#include "storage/chunk_helper.h"

namespace starrocks::pipeline {

NLJoinProbeOperator::NLJoinProbeOperator(OperatorFactory* factory, int32_t id, int32_t plan_node_id,
                                         int32_t driver_sequence, TJoinOp::type join_op,
                                         const std::string& sql_join_conjuncts,
                                         const std::vector<ExprContext*>& join_conjuncts,
                                         const std::vector<ExprContext*>& conjunct_ctxs,
                                         const std::vector<SlotDescriptor*>& col_types, size_t probe_column_count,
                                         size_t build_column_count,
                                         const std::shared_ptr<CrossJoinContext>& cross_join_context)
        : OperatorWithDependency(factory, id, "nestloop_join_probe", plan_node_id, driver_sequence),
          _join_op(join_op),
          _col_types(col_types),
          _probe_column_count(probe_column_count),
          _build_column_count(build_column_count),
          _sql_join_conjuncts(sql_join_conjuncts),
          _join_conjuncts(join_conjuncts),
          _conjunct_ctxs(conjunct_ctxs),
          _cross_join_context(cross_join_context) {
    _cross_join_context->ref();
}

Status NLJoinProbeOperator::prepare(RuntimeState* state) {
    _runtime_state = state;
    _output_accumulator.set_desired_size(state->chunk_size());

    _unique_metrics->add_info_string("join_conjuncts", _sql_join_conjuncts);
    return Operator::prepare(state);
}

void NLJoinProbeOperator::close(RuntimeState* state) {
    _cross_join_context->unref(state);
    Operator::close(state);
}

bool NLJoinProbeOperator::is_ready() const {
    return _cross_join_context->is_right_finished();
}

bool NLJoinProbeOperator::_is_curr_probe_chunk_finished() const {
    return _probe_chunk == nullptr || _probe_row_current >= _probe_chunk->num_rows();
}

void NLJoinProbeOperator::_advance_join_stage(JoinStage stage) const {
    DCHECK_LE(_join_stage, stage) << "current=" << _join_stage << ", advance to " << stage;
    if (_join_stage != stage) {
        _join_stage = stage;
        VLOG(3) << fmt::format("operator {} enter join_stage {}", _driver_sequence, stage);
    }
}

bool NLJoinProbeOperator::_skip_probe() const {
    // Empty build tbale could skip probe unless it's LEFT JOIN
    return is_ready() && !_is_left_join() && _cross_join_context->is_build_chunk_empty();
}

void NLJoinProbeOperator::_check_post_probe() const {
    bool skip_probe = _skip_probe();
    bool output_finished = _is_curr_probe_chunk_finished() && _output_accumulator.empty();

    if ((_input_finished && output_finished) || skip_probe) {
        switch (_join_stage) {
        case Probe: {
            if (!_is_right_join() || !_cross_join_context->finish_probe(_driver_sequence, _self_build_match_flag)) {
                _advance_join_stage(JoinStage::Finished);
            } else {
                _advance_join_stage(JoinStage::RightJoin);
            }
            break;
        }
        case RightJoin:
            // It should be advanced to PostRightJoin
            break;
        case PostRightJoin: {
            if (output_finished) {
                _advance_join_stage(JoinStage::Finished);
            }
            break;
        }
        case Finished:
            break;
        }
    }
}

bool NLJoinProbeOperator::has_output() const {
    _check_post_probe();
    return _join_stage != JoinStage::Finished;
}

bool NLJoinProbeOperator::need_input() const {
    if (!is_ready()) {
        return false;
    }
    if (_skip_probe()) {
        return false;
    }

    return _is_curr_probe_chunk_finished();
}

bool NLJoinProbeOperator::is_finished() const {
    return (_input_finished || _skip_probe()) && !has_output();
}

Status NLJoinProbeOperator::set_finishing(RuntimeState* state) {
    _check_post_probe();
    _input_finished = true;

    return Status::OK();
}

Status NLJoinProbeOperator::set_finished(RuntimeState* state) {
    _cross_join_context->set_finished();
    return Status::OK();
}

bool NLJoinProbeOperator::_is_left_join() const {
    return _join_op == TJoinOp::LEFT_OUTER_JOIN || _join_op == TJoinOp::FULL_OUTER_JOIN;
}

bool NLJoinProbeOperator::_is_right_join() const {
    return _join_op == TJoinOp::RIGHT_OUTER_JOIN || _join_op == TJoinOp::FULL_OUTER_JOIN;
}

int NLJoinProbeOperator::_num_build_chunks() const {
    return _cross_join_context->num_build_chunks();
}

vectorized::Chunk* NLJoinProbeOperator::_move_build_chunk_index(int index) {
    DCHECK_GE(index, 0);
    DCHECK_LE(index, _num_build_chunks());
    if (index < _num_build_chunks()) {
        _curr_build_chunk = _cross_join_context->get_build_chunk(index);
    } else {
        _curr_build_chunk = nullptr;
    }
    _curr_build_chunk_index = index;
    return _curr_build_chunk;
}

ChunkPtr NLJoinProbeOperator::_init_output_chunk(RuntimeState* state) const {
    ChunkPtr chunk = std::make_shared<vectorized::Chunk>();

    for (size_t i = 0; i < _col_types.size(); i++) {
        SlotDescriptor* slot = _col_types[i];
        bool is_probe = i < _probe_column_count;
        bool nullable = _col_types[i]->is_nullable();
        if ((is_probe && _is_right_join()) || (!is_probe && _is_left_join())) {
            nullable = true;
        }
        if (is_probe && _probe_chunk) {
            nullable |= _probe_chunk->get_column_by_slot_id(slot->id())->is_nullable();
        }
        if (!is_probe && _curr_build_chunk) {
            nullable |= _curr_build_chunk->get_column_by_slot_id(slot->id())->is_nullable();
        }
        ColumnPtr new_col = vectorized::ColumnHelper::create_column(slot->type(), nullable);
        chunk->append_column(new_col, slot->id());
    }

    chunk->reserve(state->chunk_size());
    return chunk;
}

Status NLJoinProbeOperator::_probe(RuntimeState* state, ChunkPtr chunk) {
    if ((_join_conjuncts.empty())) {
        return Status::OK();
    }
    vectorized::FilterPtr filter;
    if (chunk && !chunk->is_empty()) {
        size_t rows = chunk->num_rows();
        RETURN_IF_ERROR(eval_conjuncts_and_in_filters(_join_conjuncts, chunk.get(), &filter));
        DCHECK(!!filter);

        // The filter has not been assigned if no rows matched
        if (chunk->num_rows() == 0) {
            filter->assign(rows, 0);
        }
    }

    if (_is_left_join()) {
        if (_num_build_chunks() == 0) {
            // Empty right table
            DCHECK_EQ(_probe_row_current, _probe_chunk->num_rows());
            _permute_left_join(state, chunk, 0, _probe_chunk->num_rows());
        } else if (_num_build_chunks() == 1) {
            // Multiple probe rows
            size_t num_build_rows = _cross_join_context->num_build_rows();
            DCHECK_GE(filter->size(), num_build_rows);
            DCHECK_LE(_probe_row_start, _probe_row_current);
            for (size_t i = 0; i < filter->size(); i += num_build_rows) {
                bool probe_matched = SIMD::contain_nonzero(*filter, i, num_build_rows);
                if (!probe_matched) {
                    size_t probe_row_index = _probe_row_start + i / num_build_rows;
                    _permute_left_join(state, chunk, probe_row_index, 1);
                }
            }
        } else {
            _probe_row_matched = _probe_row_matched || SIMD::contain_nonzero(*filter);
            bool probe_row_finished = _curr_build_chunk_index >= _num_build_chunks();
            if (!_probe_row_matched && probe_row_finished) {
                _permute_left_join(state, chunk, _probe_row_current, 1);
            }
        }
    }

    if (_is_right_join() && filter) {
        VLOG(3) << fmt::format("NLJoin operator {} set build_flags for right join: {}", _driver_sequence,
                               fmt::join(*filter, ","));
        bool multi_probe_rows = _num_build_chunks() == 1;
        if (multi_probe_rows) {
            size_t num_build_rows = _cross_join_context->num_build_rows();
            DCHECK_GE(filter->size(), num_build_rows);
            for (size_t i = 0; i < filter->size(); i += num_build_rows) {
                vectorized::ColumnHelper::or_two_filters(&_self_build_match_flag, filter->data() + i);
            }
        } else {
            int flag_start = _cross_join_context->get_build_chunk_start(_curr_build_chunk_index);
            int flag_count = _curr_build_chunk->num_rows();
            vectorized::ColumnHelper::or_two_filters(flag_count, _self_build_match_flag.data() + flag_start,
                                                     filter->data());
        }
    }

    return Status::OK();
}

// Permute enough rows from build side and probe side
// The chunk either consists two conditions:
// 1. Multiple probe rows and multiple build single-chunk
// 2. One probe rows and one build chunk
ChunkPtr NLJoinProbeOperator::_permute_chunk(RuntimeState* state) {
    // TODO: optimize the loop order for small build chunk
    ChunkPtr chunk = _init_output_chunk(state);
    _probe_row_start = _probe_row_current;
    for (; _probe_row_current < _probe_chunk->num_rows(); ++_probe_row_current) {
        while (_curr_build_chunk_index < _num_build_chunks()) {
            _permute_probe_row(state, chunk);
            _move_build_chunk_index(_curr_build_chunk_index + 1);
            if (chunk->num_rows() >= state->chunk_size()) {
                return chunk;
            }
        }
        _probe_row_matched = false;
        _move_build_chunk_index(0);
    }
    return chunk;
}

// Permute one probe row with current build chunk
void NLJoinProbeOperator::_permute_probe_row(RuntimeState* state, ChunkPtr chunk) {
    DCHECK(_curr_build_chunk);
    size_t cur_build_chunk_rows = _curr_build_chunk->num_rows();
    for (size_t i = 0; i < _col_types.size(); i++) {
        bool is_probe = i < _probe_column_count;
        SlotDescriptor* slot = _col_types[i];
        ColumnPtr& dst_col = chunk->get_column_by_slot_id(slot->id());
        // TODO: specialize for null column and const column
        if (is_probe) {
            ColumnPtr& src_col = _probe_chunk->get_column_by_slot_id(slot->id());
            dst_col->append_value_multiple_times(*src_col, _probe_row_current, cur_build_chunk_rows);
        } else {
            ColumnPtr& src_col = _curr_build_chunk->get_column_by_slot_id(slot->id());
            dst_col->append(*src_col);
        }
    }
}

// Permute probe side for left join
void NLJoinProbeOperator::_permute_left_join(RuntimeState* state, ChunkPtr chunk, size_t probe_row_index,
                                             size_t probe_rows) {
    for (size_t i = 0; i < _col_types.size(); i++) {
        SlotDescriptor* slot = _col_types[i];
        ColumnPtr& dst_col = chunk->get_column_by_slot_id(slot->id());
        bool is_probe = i < _probe_column_count;
        if (is_probe) {
            ColumnPtr& src_col = _probe_chunk->get_column_by_slot_id(slot->id());
            DCHECK_LT(probe_row_index, src_col->size());
            dst_col->append(*src_col, probe_row_index, probe_rows);
        } else {
            dst_col->append_nulls(probe_rows);
        }
    }
}

// Permute build side for right join
Status NLJoinProbeOperator::_permute_right_join(RuntimeState* state) {
    const std::vector<uint8_t>& build_match_flag = _cross_join_context->get_shared_build_match_flag();
    if (!SIMD::contain_zero(build_match_flag)) {
        return Status::OK();
    }

    size_t match_flag_index = 0;
    for (int chunk_index = 0; chunk_index < _num_build_chunks(); chunk_index++) {
        _move_build_chunk_index(chunk_index);
        size_t chunk_size = _curr_build_chunk->num_rows();

        ChunkPtr chunk = _init_output_chunk(state);
        for (size_t col = 0; col < _col_types.size(); col++) {
            SlotDescriptor* slot = _col_types[col];
            ColumnPtr& dst_col = chunk->get_column_by_slot_id(slot->id());
            bool is_probe = col < _probe_column_count;
            if (is_probe) {
                size_t nonmatched_count = SIMD::count_zero(build_match_flag.data() + match_flag_index, chunk_size);
                if (nonmatched_count > 0) {
                    dst_col->append_nulls(nonmatched_count);
                }
            } else {
                ColumnPtr& src_col = _curr_build_chunk->get_column_by_slot_id(slot->id());
                for (int i = 0; i < chunk_size; i++) {
                    if (!build_match_flag[match_flag_index + i]) {
                        dst_col->append(*src_col, i, 1);
                    }
                }
            }
        }

        RETURN_IF_ERROR(eval_conjuncts(_conjunct_ctxs, chunk.get(), nullptr));
        RETURN_IF_ERROR(_output_accumulator.push(std::move(chunk)));
        match_flag_index += chunk_size;
    }

    return Status::OK();
}

// Nestloop Join algorithm:
// 1. Permute chunk from build side and probe side, until chunk size reach 4096
// 2. Apply the conjuncts, and append it to output buffer
// 3. Maintain match index and implement left join and right join
StatusOr<vectorized::ChunkPtr> NLJoinProbeOperator::pull_chunk(RuntimeState* state) {
    switch (_join_stage) {
    case Probe:
        break;
    case RightJoin: {
        DCHECK(_is_right_join());
        VLOG(3) << fmt::format("operator {} permute right_join", _driver_sequence);
        RETURN_IF_ERROR(_permute_right_join(state));
        _output_accumulator.finalize();
        _advance_join_stage(JoinStage::PostRightJoin);
        break;
    }
    case PostRightJoin:
        break;
    case Finished:
        return nullptr;
    }

    if (ChunkPtr chunk = _output_accumulator.pull()) {
        return chunk;
    }
    while (_probe_chunk && _probe_row_current < _probe_chunk->num_rows()) {
        ChunkPtr chunk = _permute_chunk(state);
        DCHECK(chunk);
        RETURN_IF_ERROR(_probe(state, chunk));
        RETURN_IF_ERROR(eval_conjuncts(_conjunct_ctxs, chunk.get(), nullptr));

        RETURN_IF_ERROR(_output_accumulator.push(std::move(chunk)));
        if (ChunkPtr res = _output_accumulator.pull()) {
            return res;
        }
    }
    _output_accumulator.finalize();

    return _output_accumulator.pull();
}

void NLJoinProbeOperator::_init_build_match() {
    if (_is_right_join() && _cross_join_context->is_right_finished() &&
        _self_build_match_flag.size() < _cross_join_context->num_build_rows()) {
        VLOG(3) << fmt::format("NLJoin operator {} init_build_match with rows {}", _driver_sequence,
                               _cross_join_context->num_build_rows());
        _self_build_match_flag.resize(_cross_join_context->num_build_rows(), 0);
    }
}

Status NLJoinProbeOperator::push_chunk(RuntimeState* state, const vectorized::ChunkPtr& chunk) {
    _init_build_match();
    _probe_chunk = chunk;
    _probe_row_start = 0;
    _probe_row_current = 0;
    _probe_row_matched = false;
    _move_build_chunk_index(0);

    return Status::OK();
}

} // namespace starrocks::pipeline