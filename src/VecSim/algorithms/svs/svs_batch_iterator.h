/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#pragma once

#include "VecSim/batch_iterator.h"
#include "VecSim/utils/vec_utils.h"
#include "VecSim/query_results.h"

#include <memory>

#include "svs/index/vamana/iterator.h"

#include "VecSim/algorithms/svs/svs_utils.h"

template <typename Index, typename DataType>
class SVS_BatchIterator : public VecSimBatchIterator {
private:
    using impl_type = svs::index::vamana::BatchIterator<Index, DataType>;
    using dist_type = typename Index::distance_type;
    size_t dim;
    std::unique_ptr<impl_type> impl_;
    decltype(impl_->begin()) curr_it;

    static std::unique_ptr<impl_type> makeImpl(const Index *index, void *query_vector,
                                               VecSimQueryParams *queryParams) {
        auto sp = svs_details::joinSearchParams(index->get_search_parameters(), queryParams);
        const size_t batch_size = queryParams && queryParams->batchSize
                                      ? queryParams->batchSize
                                      : sp.buffer_config_.get_search_window_size();
        // Base search parameters for the iterator schedule.
        auto schedule = svs::index::vamana::DefaultSchedule{sp, batch_size};
        std::span<const DataType> query{reinterpret_cast<DataType *>(query_vector),
                                        index->dimensions()};

        auto timeoutCtx = queryParams ? queryParams->timeoutCtx : nullptr;
        auto cancel = [timeoutCtx]() { return VECSIM_TIMEOUT(timeoutCtx); };
        return std::make_unique<svs::index::vamana::BatchIterator<Index, DataType>>(
            *index, query, schedule, cancel);
    }

    VecSimQueryReply *getNextResultsImpl(size_t n_res) {
        auto rep = new VecSimQueryReply(this->allocator);
        rep->results.reserve(n_res);
        auto timeoutCtx = this->getTimeoutCtx();
        auto cancel = [timeoutCtx]() { return VECSIM_TIMEOUT(timeoutCtx); };

        if (cancel()) {
            rep->code = VecSim_QueryReply_TimedOut;
            return rep;
        }

        for (size_t i = 0; i < n_res; i++) {
            if (curr_it == impl_->end()) {
                impl_->next(cancel);
                if (cancel()) {
                    rep->code = VecSim_QueryReply_TimedOut;
                    rep->results.clear();
                    return rep;
                }
                curr_it = impl_->begin();
                if (impl_->size() == 0) {
                    return rep;
                }
            }
            rep->results.push_back(VecSimQueryResult{
                curr_it->id(), svs_details::toVecSimDistance<dist_type>(curr_it->distance())});
            ++curr_it;
        }
        return rep;
    }

public:
    SVS_BatchIterator(void *query_vector, const Index *index, VecSimQueryParams *queryParams,
                      std::shared_ptr<VecSimAllocator> allocator)
        : VecSimBatchIterator{query_vector, queryParams ? queryParams->timeoutCtx : nullptr,
                              std::move(allocator)},
          dim{index->dimensions()}, impl_{makeImpl(index, query_vector, queryParams)} {
        curr_it = impl_->begin();
    }

    VecSimQueryReply *getNextResults(size_t n_res, VecSimQueryReply_Order order) override {
        auto rep = getNextResultsImpl(n_res);
        this->updateResultsCount(VecSimQueryReply_Len(rep));
        sort_results(rep, order);
        return rep;
    }

    bool isDepleted() override { return curr_it == impl_->end() && impl_->done(); }

    void reset() override {
        std::span<const DataType> query{reinterpret_cast<const DataType *>(this->getQueryBlob()),
                                        dim};
        auto timeoutCtx = this->getTimeoutCtx();
        auto cancel = [timeoutCtx]() { return VECSIM_TIMEOUT(timeoutCtx); };
        impl_->update(query, cancel);
        curr_it = impl_->begin();
    }
};

// Empty index iterator
class NullSVS_BatchIterator : public VecSimBatchIterator {
private:
public:
    NullSVS_BatchIterator(void *query_vector, VecSimQueryParams *queryParams,
                          std::shared_ptr<VecSimAllocator> allocator)
        : VecSimBatchIterator{query_vector, queryParams ? queryParams->timeoutCtx : nullptr,
                              allocator} {}

    VecSimQueryReply *getNextResults(size_t n_res, VecSimQueryReply_Order order) override {
        return new VecSimQueryReply(this->allocator);
    }

    bool isDepleted() override { return true; }

    void reset() override {}
};
