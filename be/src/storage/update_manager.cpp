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

#include "storage/update_manager.h"

#include <limits>
#include <memory>
#include <numeric>

#include "gutil/endian.h"
#include "storage/chunk_helper.h"
#include "storage/del_vector.h"
#include "storage/kv_store.h"
#include "storage/persistent_index_compaction_manager.h"
#include "storage/persistent_index_load_executor.h"
#include "storage/rowset_column_update_state.h"
#include "storage/storage_engine.h"
#include "storage/tablet.h"
#include "storage/tablet_meta_manager.h"
#include "util/failpoint/fail_point.h"
#include "util/pretty_printer.h"
#include "util/starrocks_metrics.h"
#include "util/time.h"

namespace starrocks {

Status LocalDelvecLoader::load(const TabletSegmentId& tsid, int64_t version, DelVectorPtr* pdelvec) {
    return StorageEngine::instance()->update_manager()->get_del_vec(_meta, tsid, version, pdelvec);
}

Status LocalDeltaColumnGroupLoader::load(const TabletSegmentId& tsid, int64_t version, DeltaColumnGroupList* pdcgs) {
    if (_meta == nullptr) {
        return Status::OK();
    }
    return StorageEngine::instance()->update_manager()->get_delta_column_group(_meta, tsid, version, pdcgs);
}

Status LocalDeltaColumnGroupLoader::load(int64_t tablet_id, RowsetId rowsetid, uint32_t segment_id, int64_t version,
                                         DeltaColumnGroupList* pdcgs) {
    if (_meta == nullptr) {
        return Status::OK();
    }
    return StorageEngine::instance()->get_delta_column_group(_meta, tablet_id, rowsetid, segment_id, INT64_MAX, pdcgs);
}

UpdateManager::UpdateManager(MemTracker* mem_tracker)
        : _index_cache(std::numeric_limits<size_t>::max()),
          _update_state_cache(std::numeric_limits<size_t>::max()),
          _update_column_state_cache(std::numeric_limits<size_t>::max()) {
    _update_mem_tracker = mem_tracker;
    int64_t preload_mem_limit = -1;
    if (_update_mem_tracker != nullptr) {
        preload_mem_limit = (int64_t)_update_mem_tracker->limit() *
                            std::max(std::min(100, config::lake_pk_preload_memory_limit_percent), 0) / 100;
    }
    _update_state_mem_tracker = std::make_unique<MemTracker>(preload_mem_limit, "rowset_update_state", mem_tracker);
    _index_cache_mem_tracker = std::make_unique<MemTracker>(-1, "index_cache", mem_tracker);
    _del_vec_cache_mem_tracker = std::make_unique<MemTracker>(-1, "del_vec_cache", mem_tracker);
    _compaction_state_mem_tracker = std::make_unique<MemTracker>(-1, "compaction_state", mem_tracker);
    _delta_column_group_cache_mem_tracker = std::make_unique<MemTracker>(-1, "delta_column_group_cache");

    _index_cache.set_mem_tracker(_index_cache_mem_tracker.get());
    _update_state_cache.set_mem_tracker(_update_state_mem_tracker.get());

    int64_t byte_limits = GlobalEnv::GetInstance()->process_mem_limit();
    int32_t update_mem_percent = std::max(std::min(100, config::update_memory_limit_percent), 0);
    _index_cache.set_capacity(byte_limits * update_mem_percent / 100);
    _update_column_state_cache.set_mem_tracker(_update_state_mem_tracker.get());
}

UpdateManager::~UpdateManager() {
    clear_cache();
    if (_compaction_state_mem_tracker) {
        _compaction_state_mem_tracker.reset();
    }
    if (_del_vec_cache_mem_tracker) {
        _del_vec_cache_mem_tracker.reset();
    }
    if (_delta_column_group_cache_mem_tracker) {
        _delta_column_group_cache_mem_tracker.reset();
    }
    if (_update_state_mem_tracker) {
        _update_state_mem_tracker.reset();
    }
    if (_index_cache_mem_tracker) {
        _index_cache_mem_tracker.reset();
    }
}

Status UpdateManager::init() {
    int max_thread_cnt = CpuInfo::num_cores();
    if (config::transaction_apply_worker_count > 0) {
        max_thread_cnt = config::transaction_apply_worker_count;
    }
    RETURN_IF_ERROR(
            ThreadPoolBuilder("update_apply")
                    .set_idle_timeout(MonoDelta::FromMilliseconds(config::transaction_apply_worker_idle_time_ms))
                    .set_min_threads(config::transaction_apply_thread_pool_num_min)
                    .set_max_threads(max_thread_cnt)
                    .build(&_apply_thread_pool));
    REGISTER_THREAD_POOL_METRICS(update_apply, _apply_thread_pool);

    int max_get_thread_cnt =
            config::get_pindex_worker_count > max_thread_cnt ? config::get_pindex_worker_count : max_thread_cnt * 2;
    RETURN_IF_ERROR(
            ThreadPoolBuilder("get_pindex").set_max_threads(max_get_thread_cnt).build(&_get_pindex_thread_pool));

    _persistent_index_compaction_mgr = std::make_unique<PersistentIndexCompactionManager>();
    RETURN_IF_ERROR(_persistent_index_compaction_mgr->init());

    _pindex_load_executor = std::make_unique<PersistentIndexLoadExecutor>();
    RETURN_IF_ERROR(_pindex_load_executor->init());
    return Status::OK();
}

void UpdateManager::stop() {
    if (_get_pindex_thread_pool) {
        _get_pindex_thread_pool->shutdown();
    }
    if (_apply_thread_pool) {
        _apply_thread_pool->shutdown();
    }
    if (_pindex_load_executor) {
        _pindex_load_executor->shutdown();
    }
}

int64_t UpdateManager::get_index_cache_expire_ms(const Tablet& tablet) const {
    const int32_t tablet_index_cache_expire_sec = tablet.tablet_meta()->get_primary_index_cache_expire_sec();
    if (tablet_index_cache_expire_sec > 0) {
        return tablet_index_cache_expire_sec * 1000;
    }
    return _cache_expire_ms;
}

Status UpdateManager::get_del_vec_in_meta(KVStore* meta, const TabletSegmentId& tsid, int64_t version,
                                          DelVector* delvec, int64_t* latest_version) {
    return TabletMetaManager::get_del_vector(meta, tsid.tablet_id, tsid.segment_id, version, delvec, latest_version);
}

Status UpdateManager::set_del_vec_in_meta(KVStore* meta, const TabletSegmentId& tsid, const DelVector& delvec) {
    // TODO: support batch transaction with tablet/rowset meta save
    return TabletMetaManager::set_del_vector(meta, tsid.tablet_id, tsid.segment_id, delvec);
}

Status UpdateManager::get_delta_column_group(KVStore* meta, const TabletSegmentId& tsid, int64_t version,
                                             DeltaColumnGroupList* dcgs) {
    StarRocksMetrics::instance()->delta_column_group_get_total.increment(1);
    {
        // find in delta column group cache
        std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
        auto itr = _delta_column_group_cache.find(tsid);
        if (itr != _delta_column_group_cache.end()) {
            StorageEngine::instance()->search_delta_column_groups_by_version(itr->second, version, dcgs);
            StarRocksMetrics::instance()->delta_column_group_get_hit_cache.increment(1);
            return Status::OK();
        }
    }
    // find from rocksdb
    DeltaColumnGroupList new_dcgs;
    RETURN_IF_ERROR(
            TabletMetaManager::get_delta_column_group(meta, tsid.tablet_id, tsid.segment_id, INT64_MAX, &new_dcgs));
    StorageEngine::instance()->search_delta_column_groups_by_version(new_dcgs, version, dcgs);
    {
        // fill delta column group cache
        std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
        bool ok = _delta_column_group_cache.insert({tsid, new_dcgs}).second;
        if (ok) {
            // insert success
            _delta_column_group_cache_mem_tracker->consume(
                    StorageEngine::instance()->delta_column_group_list_memory_usage(new_dcgs));
        }
    }
    return Status::OK();
}

Status UpdateManager::get_del_vec(KVStore* meta, const TabletSegmentId& tsid, int64_t version, DelVectorPtr* pdelvec) {
    {
        std::lock_guard<std::mutex> lg(_del_vec_cache_lock);
        auto itr = _del_vec_cache.find(tsid);
        if (itr != _del_vec_cache.end()) {
            if (version >= itr->second->version()) {
                VLOG(3) << strings::Substitute("get_del_vec cached tablet_segment=$0 version=$1 actual_version=$2",
                                               tsid.to_string(), version, itr->second->version());
                // cache valid
                // TODO(cbl): add cache hit stats
                *pdelvec = itr->second;
                return Status::OK();
            }
        }
    }
    (*pdelvec).reset(new DelVector());
    int64_t latest_version = 0;
    RETURN_IF_ERROR(get_del_vec_in_meta(meta, tsid, version, pdelvec->get(), &latest_version));
    if ((*pdelvec)->version() == latest_version) {
        std::lock_guard<std::mutex> lg(_del_vec_cache_lock);
        auto itr = _del_vec_cache.find(tsid);
        if (itr == _del_vec_cache.end()) {
            _del_vec_cache.emplace(tsid, *pdelvec);
            _del_vec_cache_mem_tracker->consume((*pdelvec)->memory_usage());
        } else if (latest_version > itr->second->version()) {
            // should happen rarely
            _del_vec_cache_mem_tracker->release(itr->second->memory_usage());
            itr->second = (*pdelvec);
            _del_vec_cache_mem_tracker->consume(itr->second->memory_usage());
        }
    }
    return Status::OK();
}

void UpdateManager::clear_cache() {
    _update_state_cache.clear();
    _update_column_state_cache.clear();
    if (_update_state_mem_tracker) {
        _update_state_mem_tracker->release(_update_state_mem_tracker->consumption());
    }
    _index_cache.clear();
    if (_index_cache_mem_tracker) {
        _index_cache_mem_tracker->release(_index_cache_mem_tracker->consumption());
    }
    StarRocksMetrics::instance()->update_primary_index_num.set_value(0);
    StarRocksMetrics::instance()->update_primary_index_bytes_total.set_value(0);
    {
        std::lock_guard<std::mutex> lg(_del_vec_cache_lock);
        _del_vec_cache.clear();
        if (_del_vec_cache_mem_tracker) {
            _del_vec_cache_mem_tracker->release(_del_vec_cache_mem_tracker->consumption());
        }
        StarRocksMetrics::instance()->update_del_vector_num.set_value(0);
        StarRocksMetrics::instance()->update_del_vector_bytes_total.set_value(0);
    }
    {
        std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
        _delta_column_group_cache.clear();
        if (_delta_column_group_cache_mem_tracker) {
            _delta_column_group_cache_mem_tracker->release(_delta_column_group_cache_mem_tracker->consumption());
        }
    }
}

void UpdateManager::clear_cached_del_vec_by_tablet_id(int64_t tablet_id) {
    std::lock_guard<std::mutex> lg(_del_vec_cache_lock);
    for (auto iter = _del_vec_cache.begin(); iter != _del_vec_cache.end();) {
        if (iter->first.tablet_id == tablet_id) {
            _del_vec_cache_mem_tracker->release(iter->second->memory_usage());
            iter = _del_vec_cache.erase(iter);
        } else {
            ++iter;
        }
    }
}

void UpdateManager::clear_cached_del_vec(const std::vector<TabletSegmentId>& tsids) {
    std::lock_guard<std::mutex> lg(_del_vec_cache_lock);
    for (const auto& tsid : tsids) {
        auto itr = _del_vec_cache.find(tsid);
        if (itr != _del_vec_cache.end()) {
            _del_vec_cache_mem_tracker->release(itr->second->memory_usage());
            _del_vec_cache.erase(itr);
        }
    }
}

StatusOr<size_t> UpdateManager::clear_delta_column_group_before_version(KVStore* meta, const std::string& tablet_path,
                                                                        int64_t tablet_id,
                                                                        int64_t min_readable_version) {
    std::vector<std::pair<TabletSegmentId, int64_t>> clear_dcgs;
    std::vector<std::string> clear_filenames;
    const int64_t begin_ms = UnixMillis();
    auto is_timeout = [begin_ms]() {
        if (UnixMillis() > begin_ms + 10) { // only hold cache_clock for 10ms max.
            return true;
        } else {
            return false;
        }
    };
    {
        std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
        auto itr = _delta_column_group_cache.lower_bound(TabletSegmentId(tablet_id, 0));
        while (itr != _delta_column_group_cache.end() && !is_timeout() && itr->first.tablet_id == tablet_id) {
            // gc not required delta column group
            DeltaColumnGroupListHelper::garbage_collection(itr->second, itr->first, min_readable_version, tablet_path,
                                                           &clear_dcgs, &clear_filenames);
            itr++;
        }
    }
    // delete dcg from rocksdb
    WriteBatch wb;
    for (const auto& dcg : clear_dcgs) {
        auto st = TabletMetaManager::delete_delta_column_group(meta, &wb, dcg.first, dcg.second);
        if (!st.ok()) {
            // continue if error
            LOG(WARNING) << "clear delta column group failed, tablet_id: " << tablet_id << " st: " << st.message();
        }
    }
    RETURN_IF_ERROR(meta->write_batch(&wb));
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(tablet_path));
    for (const auto& filename : clear_filenames) {
        WARN_IF_ERROR(fs->delete_file(filename), "delete file fail, filename: " + filename);
    }
    return clear_dcgs.size();
}

void UpdateManager::clear_cached_delta_column_group_by_tablet_id(int64_t tablet_id) {
    std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
    for (auto iter = _delta_column_group_cache.begin(); iter != _delta_column_group_cache.end();) {
        if (iter->first.tablet_id == tablet_id) {
            _delta_column_group_cache_mem_tracker->release(
                    StorageEngine::instance()->delta_column_group_list_memory_usage(iter->second));
            iter = _delta_column_group_cache.erase(iter);
        } else {
            ++iter;
        }
    }
}

void UpdateManager::clear_cached_delta_column_group(const std::vector<TabletSegmentId>& tsids) {
    std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
    for (const auto& tsid : tsids) {
        auto itr = _delta_column_group_cache.find(tsid);
        if (itr != _delta_column_group_cache.end()) {
            _delta_column_group_cache_mem_tracker->release(
                    StorageEngine::instance()->delta_column_group_list_memory_usage(itr->second));
            _delta_column_group_cache.erase(itr);
        }
    }
}

Status UpdateManager::set_cached_empty_delta_column_group(KVStore* meta, const TabletSegmentId& tsid) {
    {
        std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
        auto itr = _delta_column_group_cache.find(tsid);
        if (itr != _delta_column_group_cache.end()) {
            // already exist, not need to cache
            return Status::OK();
        }
    }
    // find from rocksdb
    DeltaColumnGroupList new_dcgs;
    RETURN_IF_ERROR(
            TabletMetaManager::get_delta_column_group(meta, tsid.tablet_id, tsid.segment_id, INT64_MAX, &new_dcgs));
    std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
    auto itr = _delta_column_group_cache.find(tsid);
    if (itr != _delta_column_group_cache.end()) {
        // already exist, not need to cache
        return Status::OK();
    }
    if (new_dcgs.empty()) {
        // only set empty dcgs
        _delta_column_group_cache[tsid] = new_dcgs;
    }
    return Status::OK();
}

bool UpdateManager::get_cached_delta_column_group(const TabletSegmentId& tsid, int64_t version,
                                                  DeltaColumnGroupList* dcgs) {
    // find in delta column group cache
    std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
    auto itr = _delta_column_group_cache.find(tsid);
    if (itr != _delta_column_group_cache.end()) {
        StorageEngine::instance()->search_delta_column_groups_by_version(itr->second, version, dcgs);
        // hit cache
        return true;
    }
    // miss cache
    return false;
}

Status UpdateManager::set_cached_delta_column_group(KVStore* meta, const TabletSegmentId& tsid,
                                                    const DeltaColumnGroupPtr& dcg) {
    {
        std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
        auto itr = _delta_column_group_cache.find(tsid);
        if (itr != _delta_column_group_cache.end()) {
            itr->second.insert(itr->second.begin(), dcg);
            _delta_column_group_cache_mem_tracker->consume(dcg->memory_usage());
            return Status::OK();
        }
    }
    // find from rocksdb
    DeltaColumnGroupList new_dcgs;
    RETURN_IF_ERROR(
            TabletMetaManager::get_delta_column_group(meta, tsid.tablet_id, tsid.segment_id, INT64_MAX, &new_dcgs));
    std::lock_guard<std::mutex> lg(_delta_column_group_cache_lock);
    auto itr = _delta_column_group_cache.find(tsid);
    if (itr != _delta_column_group_cache.end()) {
        _delta_column_group_cache_mem_tracker->release(
                StorageEngine::instance()->delta_column_group_list_memory_usage(itr->second));
    }
    _delta_column_group_cache[tsid] = new_dcgs;
    _delta_column_group_cache_mem_tracker->consume(
            StorageEngine::instance()->delta_column_group_list_memory_usage(new_dcgs));
    return Status::OK();
}

void UpdateManager::expire_cache() {
    StarRocksMetrics::instance()->update_primary_index_num.set_value(_index_cache.object_size());
    StarRocksMetrics::instance()->update_primary_index_bytes_total.set_value(_index_cache.size());
    {
        std::lock_guard<std::mutex> lg(_del_vec_cache_lock);
        StarRocksMetrics::instance()->update_del_vector_num.set_value(_del_vec_cache.size());
        StarRocksMetrics::instance()->update_del_vector_bytes_total.set_value(std::accumulate(
                _del_vec_cache.cbegin(), _del_vec_cache.cend(), 0,
                [](const int& accumulated, const auto& p) { return accumulated + p.second->memory_usage(); }));
    }
    if (MonotonicMillis() - _last_clear_expired_cache_millis > _cache_expire_ms) {
        _update_state_cache.clear_expired();
        _update_column_state_cache.clear_expired();

        ssize_t orig_size = _index_cache.size();
        ssize_t orig_obj_size = _index_cache.object_size();
        _index_cache.clear_expired();
        ssize_t size = _index_cache.size();
        ssize_t obj_size = _index_cache.object_size();
        LOG(INFO) << strings::Substitute("index cache expire: before:($0 $1) after:($2 $3) expire: ($4 $5)",
                                         orig_obj_size, PrettyPrinter::print_bytes(orig_size), obj_size,
                                         PrettyPrinter::print_bytes(size), orig_obj_size - obj_size,
                                         PrettyPrinter::print_bytes(orig_size - size));

        _last_clear_expired_cache_millis = MonotonicMillis();
    }
}

void UpdateManager::evict_cache(int64_t memory_urgent_level, int64_t memory_high_level) {
    int64_t capacity = _index_cache.capacity();
    int64_t size = _index_cache.size();
    int64_t memory_urgent = capacity * memory_urgent_level / 100;
    int64_t memory_high = capacity * memory_high_level / 100;

    if (size > memory_urgent) {
        _index_cache.try_evict(memory_urgent);
    }

    size = _index_cache.size();
    if (size > memory_high) {
        int64_t target_memory = std::max((size * 9 / 10), memory_high);
        _index_cache.try_evict(target_memory);
    }
    _keep_pindex_bf = _index_cache.size() > memory_high ? false : true;
    return;
}

string UpdateManager::memory_stats() {
    return strings::Substitute("index:$0 rowset:$1 compaction:$2 delvec:$3 dcg:$4 total:$5/$6",
                               PrettyPrinter::print_bytes(_index_cache_mem_tracker->consumption()),
                               PrettyPrinter::print_bytes(_update_state_mem_tracker->consumption()),
                               PrettyPrinter::print_bytes(_compaction_state_mem_tracker->consumption()),
                               PrettyPrinter::print_bytes(_del_vec_cache_mem_tracker->consumption()),
                               PrettyPrinter::print_bytes(_delta_column_group_cache_mem_tracker->consumption()),
                               PrettyPrinter::print_bytes(_update_mem_tracker->consumption()),
                               PrettyPrinter::print_bytes(_update_mem_tracker->limit()));
}

string UpdateManager::detail_memory_stats() {
    auto primary_index_stats = _index_cache.get_entry_sizes();
    std::sort(primary_index_stats.begin(), primary_index_stats.end(),
              [](const std::pair<uint64_t, size_t>& lhs, const std::pair<uint64_t, size_t>& rhs) {
                  return lhs.second > rhs.second;
              });
    size_t total_memory = 0;
    for (const auto& e : primary_index_stats) {
        total_memory += e.second;
    }
    string ret;
    StringAppendF(&ret, "primary index stats: total:%zu memory:%zu\n  tabletid       memory\n",
                  primary_index_stats.size(), total_memory);
    for (size_t i = 0; i < std::min(primary_index_stats.size(), (size_t)200); i++) {
        auto& e = primary_index_stats[i];
        StringAppendF(&ret, "%10lu %12zu\n", (unsigned long)e.first, e.second);
    }
    return ret;
}

string UpdateManager::topn_memory_stats(size_t topn) {
    auto primary_index_stats = _index_cache.get_entry_sizes();
    std::sort(primary_index_stats.begin(), primary_index_stats.end(),
              [](const std::pair<uint64_t, size_t>& lhs, const std::pair<uint64_t, size_t>& rhs) {
                  return lhs.second > rhs.second;
              });
    string ret;
    for (size_t i = 0; i < std::min(primary_index_stats.size(), topn); i++) {
        auto& e = primary_index_stats[i];
        StringAppendF(&ret, "%lu(%zuM)", (unsigned long)e.first, e.second / (1024 * 1024));
    }
    return ret;
}

Status UpdateManager::get_latest_del_vec(KVStore* meta, const TabletSegmentId& tsid, DelVectorPtr* pdelvec) {
    std::lock_guard<std::mutex> lg(_del_vec_cache_lock);
    auto itr = _del_vec_cache.find(tsid);
    if (itr != _del_vec_cache.end()) {
        *pdelvec = itr->second;
        return Status::OK();
    } else {
        // TODO(cbl): move get_del_vec_in_meta out of lock
        (*pdelvec).reset(new DelVector());
        int64_t latest_version = 0;
        RETURN_IF_ERROR(get_del_vec_in_meta(meta, tsid, INT64_MAX, pdelvec->get(), &latest_version));
        _del_vec_cache.emplace(tsid, *pdelvec);
        _del_vec_cache_mem_tracker->consume((*pdelvec)->memory_usage());
    }
    return Status::OK();
}

Status UpdateManager::set_cached_del_vec(const TabletSegmentId& tsid, const DelVectorPtr& delvec) {
    VLOG(2) << "set_cached_del_vec tablet:" << tsid.tablet_id << " rss:" << tsid.segment_id
            << " version:" << delvec->version() << " #del:" << delvec->cardinality();
    std::lock_guard<std::mutex> lg(_del_vec_cache_lock);
    auto itr = _del_vec_cache.find(tsid);
    if (itr != _del_vec_cache.end()) {
        if (delvec->version() <= itr->second->version()) {
            string msg = strings::Substitute("UpdateManager::set_cached_del_vec: new version($0) < old version($1)",
                                             delvec->version(), itr->second->version());
            LOG(ERROR) << msg;
            return Status::InternalError(msg);
        } else {
            _del_vec_cache_mem_tracker->release(itr->second->memory_usage());
            itr->second = delvec;
            _del_vec_cache_mem_tracker->consume(itr->second->memory_usage());
        }
    } else {
        _del_vec_cache.emplace(tsid, delvec);
        _del_vec_cache_mem_tracker->consume(delvec->memory_usage());
    }
    return Status::OK();
}

DEFINE_FAIL_POINT(on_rowset_finished_failed_due_to_mem);
Status UpdateManager::on_rowset_finished(Tablet* tablet, Rowset* rowset) {
    SCOPED_THREAD_LOCAL_MEM_SETTER(GlobalEnv::GetInstance()->process_mem_tracker(), true);
    SCOPED_THREAD_LOCAL_SINGLETON_CHECK_MEM_TRACKER_SETTER(config::enable_pk_strict_memcheck ? mem_tracker() : nullptr);
    if (!rowset->has_data_files() || tablet->tablet_state() == TABLET_NOTREADY) {
        // if rowset is empty or tablet is in schemachange, we can skip preparing updatestates and pre-loading primary index
        return Status::OK();
    }

    string rowset_unique_id = rowset->rowset_id().to_string();
    VLOG(2) << "UpdateManager::on_rowset_finished start tablet:" << tablet->tablet_id()
            << " rowset:" << rowset_unique_id;
    // Prepare apply required resources, load updatestate, primary index into cache,
    // so apply can run faster. Since those resources are in cache, they can get evicted
    // before used in apply process, in that case, these will be loaded again in apply
    // process.

    if (rowset->is_partial_update()) {
        auto task_st = _pindex_load_executor->submit_task_and_wait_for(
                std::static_pointer_cast<Tablet>(tablet->shared_from_this()), config::pindex_rebuild_load_wait_seconds);
        if (!task_st.ok()) {
            return Status::Uninitialized(task_st.message());
        }
    }

    Status st;
    if (rowset->is_column_mode_partial_update()) {
        auto state_entry = _update_column_state_cache.get_or_create(
                strings::Substitute("$0_$1", tablet->tablet_id(), rowset_unique_id));
        st = state_entry->value().load(tablet, rowset, _update_mem_tracker);
        state_entry->update_expire_time(MonotonicMillis() + _cache_expire_ms);
        _update_column_state_cache.update_object_size(state_entry, state_entry->value().memory_usage());
        if (st.ok()) {
            _update_column_state_cache.release(state_entry);
        } else {
            if (st.is_mem_limit_exceeded() || st.is_time_out()) {
                VLOG(2) << "load RowsetColumnUpdateState error: " << st << " tablet: " << tablet->tablet_id();
            } else {
                LOG(WARNING) << "load RowsetColumnUpdateState error: " << st << " tablet: " << tablet->tablet_id();
            }
            _update_column_state_cache.remove(state_entry);
        }
    } else {
        auto state_entry =
                _update_state_cache.get_or_create(strings::Substitute("$0_$1", tablet->tablet_id(), rowset_unique_id));
        st = state_entry->value().load(tablet, rowset);
        state_entry->update_expire_time(MonotonicMillis() + _cache_expire_ms);
        _update_state_cache.update_object_size(state_entry, state_entry->value().memory_usage());
        if (st.ok()) {
            _update_state_cache.release(state_entry);
        } else {
            if (st.is_mem_limit_exceeded() || st.is_time_out()) {
                VLOG(2) << "load RowsetUpdateState error: " << st << " tablet: " << tablet->tablet_id();
            } else {
                LOG(WARNING) << "load RowsetUpdateState error: " << st << " tablet: " << tablet->tablet_id();
            }
            _update_state_cache.remove(state_entry);
        }
    }

    // tablet maybe dropped during ingestion, add some log
    if (!st.ok()) {
        if (tablet->tablet_state() == TABLET_SHUTDOWN) {
            std::string msg = strings::Substitute("tablet $0 in TABLET_SHUTDOWN, maybe deleted by other thread",
                                                  tablet->tablet_id());
            LOG(WARNING) << msg;
        }
    }

    VLOG(2) << "UpdateManager::on_rowset_finished finish tablet:" << tablet->tablet_id()
            << " rowset:" << rowset_unique_id;

    FAIL_POINT_TRIGGER_EXECUTE(on_rowset_finished_failed_due_to_mem,
                               { st = Status::MemoryLimitExceeded("on_rowset_finished failed"); });
    // if failed due to memory limit or wait index lock timeout which is not a critical issue.
    // we don't need to abort the ingestion and we can still commit the txn.
    if (st.is_mem_limit_exceeded() || st.is_time_out()) {
        return Status::OK();
    }
    return st;
}

void UpdateManager::on_rowset_cancel(Tablet* tablet, Rowset* rowset) {
    string rowset_unique_id = rowset->rowset_id().to_string();
    VLOG(2) << "UpdateManager::on_rowset_error remove state tablet:" << tablet->tablet_id()
            << " rowset:" << rowset_unique_id;
    if (rowset->is_column_mode_partial_update()) {
        auto column_state_entry =
                _update_column_state_cache.get(strings::Substitute("$0_$1", tablet->tablet_id(), rowset_unique_id));
        if (column_state_entry != nullptr) {
            _update_column_state_cache.remove(column_state_entry);
        }
    } else {
        auto state_entry = _update_state_cache.get(strings::Substitute("$0_$1", tablet->tablet_id(), rowset_unique_id));
        if (state_entry != nullptr) {
            _update_state_cache.remove(state_entry);
        }
    }
}

bool UpdateManager::TEST_update_state_exist(Tablet* tablet, Rowset* rowset) {
    string rowset_unique_id = rowset->rowset_id().to_string();
    if (rowset->is_column_mode_partial_update()) {
        auto column_state_entry =
                _update_column_state_cache.get(strings::Substitute("$0_$1", tablet->tablet_id(), rowset_unique_id));
        if (column_state_entry != nullptr) {
            _update_column_state_cache.release(column_state_entry);
            return true;
        }
    } else {
        auto state_entry = _update_state_cache.get(strings::Substitute("$0_$1", tablet->tablet_id(), rowset_unique_id));
        if (state_entry != nullptr) {
            _update_state_cache.release(state_entry);
            return true;
        }
    }
    return false;
}

bool UpdateManager::TEST_primary_index_refcnt(int64_t tablet_id, uint32_t expected_cnt) {
    auto index_entry = _index_cache.get(tablet_id);
    if (index_entry == nullptr) {
        return expected_cnt == 0;
    }
    _index_cache.release(index_entry);
    return index_entry->get_ref() == expected_cnt;
}

} // namespace starrocks
