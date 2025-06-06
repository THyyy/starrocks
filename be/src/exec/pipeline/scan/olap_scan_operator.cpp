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

#include "exec/pipeline/scan/olap_scan_operator.h"

#include "column/chunk.h"
#include "exec/olap_scan_node.h"
#include "exec/pipeline/scan/olap_chunk_source.h"
#include "exec/pipeline/scan/olap_scan_context.h"
#include "fmt/format.h"
#include "runtime/current_thread.h"
#include "runtime/exec_env.h"
#include "runtime/runtime_state.h"
#include "storage/storage_engine.h"

namespace starrocks::pipeline {

// ==================== OlapScanOperatorFactory ====================

OlapScanOperatorFactory::OlapScanOperatorFactory(int32_t id, ScanNode* scan_node, OlapScanContextFactoryPtr ctx_factory)
        : ScanOperatorFactory(id, scan_node), _ctx_factory(std::move(ctx_factory)) {}

Status OlapScanOperatorFactory::do_prepare(RuntimeState* state) {
    auto olap_scan_node = dynamic_cast<OlapScanNode*>(_scan_node);
    DCHECK(olap_scan_node != nullptr);
    const TOlapScanNode& thrift_olap_scan_node = olap_scan_node->thrift_olap_scan_node();
    const TupleDescriptor* tuple_desc = state->desc_tbl().get_tuple_descriptor(thrift_olap_scan_node.tuple_id);
    DCHECK(tuple_desc != nullptr);
    _ctx_factory->set_scan_table_id(tuple_desc->table_desc()->table_id());
    return Status::OK();
}

void OlapScanOperatorFactory::do_close(RuntimeState*) {}

OperatorPtr OlapScanOperatorFactory::do_create(int32_t dop, int32_t driver_sequence) {
    return std::make_shared<OlapScanOperator>(this, _id, driver_sequence, dop, _scan_node,
                                              _ctx_factory->get_or_create(driver_sequence));
}

const std::vector<ExprContext*>& OlapScanOperatorFactory::partition_exprs() const {
    auto* olap_scan_node = down_cast<OlapScanNode*>(_scan_node);
    return olap_scan_node->bucket_exprs();
}

// ==================== OlapScanOperator ====================

OlapScanOperator::OlapScanOperator(OperatorFactory* factory, int32_t id, int32_t driver_sequence, int32_t dop,
                                   ScanNode* scan_node, OlapScanContextPtr ctx)
        : ScanOperator(factory, id, driver_sequence, dop, scan_node), _ctx(std::move(ctx)) {
    _ctx->ref();
}

OlapScanOperator::~OlapScanOperator() {
    auto* state = runtime_state();
    if (state == nullptr) {
        return;
    }

    _ctx->unref(state);
}

bool OlapScanOperator::has_output() const {
    if (!_ctx->is_prepare_finished() || _ctx->is_finished()) {
        return false;
    }

    return ScanOperator::has_output();
}

bool OlapScanOperator::is_finished() const {
    if (_ctx->is_finished() || _is_finished) {
        return true;
    }

    // ScanOperator::is_finished() will check whether the morsel queue has more morsels,
    // and some kinds of morsel queue will be ready after the scan context prepares ready.
    // Therefore, return false when the context is not ready.
    if (!_ctx->is_prepare_finished()) {
        return false;
    }

    return ScanOperator::is_finished();
}

Status OlapScanOperator::do_prepare(RuntimeState* state) {
    bool shared_scan = _ctx->is_shared_scan();
    _unique_metrics->add_info_string("SharedScan", shared_scan ? "True" : "False");
    _ctx->attach_observer(state, observer());
    return Status::OK();
}

void OlapScanOperator::do_close(RuntimeState* state) {}

ChunkSourcePtr OlapScanOperator::create_chunk_source(MorselPtr morsel, int32_t chunk_source_index) {
    auto* olap_scan_node = down_cast<OlapScanNode*>(_scan_node);
    return std::make_shared<OlapChunkSource>(this, _chunk_source_profiles[chunk_source_index].get(), std::move(morsel),
                                             olap_scan_node, _ctx.get());
}

int64_t OlapScanOperator::get_scan_table_id() const {
    return _ctx->get_scan_table_id();
}

void OlapScanOperator::attach_chunk_source(int32_t source_index) {
    _ctx->attach_shared_input(_driver_sequence, source_index);
}

void OlapScanOperator::detach_chunk_source(int32_t source_index) {
    _ctx->detach_shared_input(_driver_sequence, source_index);
}

bool OlapScanOperator::has_shared_chunk_source() const {
    return _ctx->has_active_input();
}

BalancedChunkBuffer& OlapScanOperator::get_chunk_buffer() const {
    return _ctx->get_chunk_buffer();
}

bool OlapScanOperator::need_notify_all() {
    return (!_ctx->only_one_observer() && _ctx->active_inputs_empty_event()) || has_full_events();
}

std::string OlapScanOperator::get_name() const {
    std::string finished = is_finished() ? "X" : "O";
    bool full = is_buffer_full();
    int io_tasks = _num_running_io_tasks;
    bool has_active = _ctx->has_active_input();
    std::string morsel_queue_name = _morsel_queue->name();
    bool morsel_queue_empty = _morsel_queue->empty();
    return fmt::format(
            "{}_{}_{}({}) {{ full:{} iostasks:{} has_active:{} num_chunks:{} morsel:{} empty:{} has_output:{}}}", _name,
            _plan_node_id, (void*)this, finished, full, io_tasks, has_active, num_buffered_chunks(), morsel_queue_name,
            morsel_queue_empty, has_output());
}

} // namespace starrocks::pipeline
