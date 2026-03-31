#pragma once

#include "isla/server/memory/conversation.hpp"
#include "isla/server/memory/memory_orchestrator.hpp"
#include "isla/server/memory/mid_term_compactor.hpp"
#include "isla/server/memory/mid_term_flush_decider.hpp"
#include "isla/server/memory/prompt_loader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "server/memory/src/mid_term/test/mid_term_compactor_mock.hpp"
#include "server/memory/src/mid_term/test/mid_term_flush_decider_mock.hpp"
#include "server/memory/src/shared/test/memory_store_mock.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace isla::server::memory {
namespace {

using nlohmann::json;
using namespace std::chrono_literals;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

inline constexpr std::size_t kImmediateMidTermFlushDeciderIntervalUserTurns = 1U;

MidTermFlushDecision NoFlushDecision() {
    return MidTermFlushDecision{
        .boundaries = {},
        .tail_complete = false,
    };
}

MidTermFlushDecision TailCompleteDecision() {
    return MidTermFlushDecision{
        .boundaries = {},
        .tail_complete = true,
    };
}

MidTermFlushDecision SplitAtDecision(std::size_t split_before_message_index,
                                     bool tail_complete = false) {
    return MidTermFlushDecision{
        .boundaries =
            {
                MidTermFlushBoundary{ .split_before_message_index = split_before_message_index },
            },
        .tail_complete = tail_complete,
    };
}

class RecordingMemoryStore final : public NiceMock<test::MockMemoryStore> {
  public:
    RecordingMemoryStore() {
        ON_CALL(*this, WarmUp()).WillByDefault(Return(absl::OkStatus()));
        ON_CALL(*this, UpsertSession(_)).WillByDefault([this](const MemorySessionRecord& record) {
            if (!upsert_session_status.ok()) {
                return upsert_session_status;
            }
            session_records.push_back(record);
            return absl::OkStatus();
        });
        ON_CALL(*this, UpsertUserWorkingMemory(_))
            .WillByDefault([this](const UserWorkingMemoryRecord& record) {
                if (!upsert_user_working_memory_status.ok()) {
                    return upsert_user_working_memory_status;
                }
                user_working_memory_records.push_back(record);
                return absl::OkStatus();
            });
        ON_CALL(*this, AppendConversationMessage(_))
            .WillByDefault([this](const ConversationMessageWrite& write) {
                if (!append_message_status.ok()) {
                    return append_message_status;
                }
                message_writes.push_back(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, ReplaceConversationItemWithEpisodeStub(_))
            .WillByDefault([this](const EpisodeStubWrite& write) {
                if (!replace_stub_status.ok()) {
                    return replace_stub_status;
                }
                stub_writes.push_back(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, SplitConversationItemWithEpisodeStub(_))
            .WillByDefault([this](const SplitEpisodeStubWrite& write) {
                if (!split_stub_status.ok()) {
                    return split_stub_status;
                }
                split_stub_writes.push_back(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, ClearSessionWorkingSet(_))
            .WillByDefault([this](std::string_view session_id) {
                if (!clear_working_set_status.ok()) {
                    return clear_working_set_status;
                }
                cleared_session_ids.push_back(std::string(session_id));
                return absl::OkStatus();
            });
        ON_CALL(*this, UpsertMidTermEpisode(_))
            .WillByDefault([this](const MidTermEpisodeWrite& write) {
                if (!upsert_episode_status.ok()) {
                    return upsert_episode_status;
                }
                episode_writes.push_back(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, PersistSleepCycleExtraction(_))
            .WillByDefault([this](const SleepCycleExtractionResult& result) {
                if (!persist_sleep_cycle_extraction_status.ok()) {
                    return persist_sleep_cycle_extraction_status;
                }
                extraction_results.push_back(result);
                entity_writes.insert(entity_writes.end(), result.entities.begin(),
                                     result.entities.end());
                relationship_writes.insert(relationship_writes.end(), result.relationships.begin(),
                                           result.relationships.end());
                long_term_episode_writes.insert(long_term_episode_writes.end(),
                                                result.long_term_episodes.begin(),
                                                result.long_term_episodes.end());
                long_term_episode_entity_links.insert(long_term_episode_entity_links.end(),
                                                      result.long_term_episode_entity_links.begin(),
                                                      result.long_term_episode_entity_links.end());
                return absl::OkStatus();
            });
        ON_CALL(*this, UpsertEntity(_)).WillByDefault([this](const EntityWrite& write) {
            if (!upsert_entity_status.ok()) {
                return upsert_entity_status;
            }
            entity_writes.push_back(write);
            return absl::OkStatus();
        });
        ON_CALL(*this, UpsertRelationship(_)).WillByDefault([this](const RelationshipWrite& write) {
            if (!upsert_relationship_status.ok()) {
                return upsert_relationship_status;
            }
            relationship_writes.push_back(write);
            return absl::OkStatus();
        });
        ON_CALL(*this, UpsertLongTermEpisode(_))
            .WillByDefault([this](const LongTermEpisodeWrite& write) {
                if (!upsert_long_term_episode_status.ok()) {
                    return upsert_long_term_episode_status;
                }
                long_term_episode_writes.push_back(write);
                return absl::OkStatus();
            });
        ON_CALL(*this, LinkLongTermEpisodeEntities(_))
            .WillByDefault([this](const LongTermEpisodeEntityLink& link) {
                if (!link_long_term_episode_entities_status.ok()) {
                    return link_long_term_episode_entities_status;
                }
                long_term_episode_entity_links.push_back(link);
                return absl::OkStatus();
            });
        ON_CALL(*this, ListEntitiesByUser(_))
            .WillByDefault([this](std::string_view user_id) -> absl::StatusOr<std::vector<Entity>> {
                static_cast<void>(user_id);
                if (!list_entities_status.ok()) {
                    return list_entities_status;
                }
                return entities;
            });
        ON_CALL(*this, GetEntity(_))
            .WillByDefault(
                [this](std::string_view entity_id) -> absl::StatusOr<std::optional<Entity>> {
                    for (const Entity& entity : entities) {
                        if (entity.entity_id == entity_id) {
                            return entity;
                        }
                    }
                    return std::nullopt;
                });
        ON_CALL(*this, ListRelationshipsForEntity(_))
            .WillByDefault(
                [this](std::string_view entity_id) -> absl::StatusOr<std::vector<Relationship>> {
                    static_cast<void>(entity_id);
                    return relationships;
                });
        ON_CALL(*this, ListLongTermEpisodesForEntity(_))
            .WillByDefault(
                [this](std::string_view entity_id) -> absl::StatusOr<std::vector<LongTermEpisode>> {
                    static_cast<void>(entity_id);
                    return long_term_episodes;
                });
        ON_CALL(*this, ListMidTermEpisodes(_))
            .WillByDefault([](std::string_view session_id) -> absl::StatusOr<std::vector<Episode>> {
                static_cast<void>(session_id);
                return std::vector<Episode>{};
            });
        ON_CALL(*this, GetMidTermEpisode(_, _))
            .WillByDefault(
                [](std::string_view session_id,
                   std::string_view episode_id) -> absl::StatusOr<std::optional<Episode>> {
                    static_cast<void>(session_id);
                    static_cast<void>(episode_id);
                    return std::nullopt;
                });
        ON_CALL(*this, LoadSnapshot(_))
            .WillByDefault([](std::string_view session_id)
                               -> absl::StatusOr<std::optional<MemoryStoreSnapshot>> {
                static_cast<void>(session_id);
                return std::nullopt;
            });
    }

    std::vector<MemorySessionRecord> session_records;
    std::vector<UserWorkingMemoryRecord> user_working_memory_records;
    std::vector<ConversationMessageWrite> message_writes;
    std::vector<EpisodeStubWrite> stub_writes;
    std::vector<SplitEpisodeStubWrite> split_stub_writes;
    std::vector<std::string> cleared_session_ids;
    std::vector<MidTermEpisodeWrite> episode_writes;
    std::vector<SleepCycleExtractionResult> extraction_results;
    std::vector<EntityWrite> entity_writes;
    std::vector<RelationshipWrite> relationship_writes;
    std::vector<LongTermEpisodeWrite> long_term_episode_writes;
    std::vector<LongTermEpisodeEntityLink> long_term_episode_entity_links;
    std::vector<Entity> entities;
    std::vector<Relationship> relationships;
    std::vector<LongTermEpisode> long_term_episodes;
    absl::Status upsert_session_status = absl::OkStatus();
    absl::Status upsert_user_working_memory_status = absl::OkStatus();
    absl::Status append_message_status = absl::OkStatus();
    absl::Status replace_stub_status = absl::OkStatus();
    absl::Status split_stub_status = absl::OkStatus();
    absl::Status clear_working_set_status = absl::OkStatus();
    absl::Status upsert_episode_status = absl::OkStatus();
    absl::Status persist_sleep_cycle_extraction_status = absl::OkStatus();
    absl::Status upsert_entity_status = absl::OkStatus();
    absl::Status upsert_relationship_status = absl::OkStatus();
    absl::Status upsert_long_term_episode_status = absl::OkStatus();
    absl::Status link_long_term_episode_entities_status = absl::OkStatus();
    absl::Status list_entities_status = absl::OkStatus();
};

std::shared_future<void> MakeReadyFuture() {
    std::promise<void> ready_promise;
    ready_promise.set_value();
    return ready_promise.get_future().share();
}

class RecordingMidTermCompactor final : public NiceMock<test::MockMidTermCompactor> {
  public:
    explicit RecordingMidTermCompactor(absl::StatusOr<CompactedMidTermEpisode> result =
                                           CompactedMidTermEpisode{
                                               .tier1_detail = std::string("full detail"),
                                               .tier2_summary = "summary",
                                               .tier3_ref = "stub ref",
                                               .tier3_keywords = { "memory" },
                                               .salience = kExpandableEpisodeSalienceThreshold,
                                               .embedding = {},
                                           },
                                       std::shared_future<void> release_signal = MakeReadyFuture())
        : result_(std::move(result)), release_signal_(std::move(release_signal)) {
        ON_CALL(*this, Compact(_)).WillByDefault([this](const MidTermCompactionRequest& request) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request);
            }
            release_signal_.wait();
            return result_;
        });
    }

    [[nodiscard]] bool WaitForRequestCount(std::size_t expected_count) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requests_.size() >= expected_count) {
                    return true;
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    [[nodiscard]] std::vector<MidTermCompactionRequest> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<MidTermCompactionRequest> requests_;
    absl::StatusOr<CompactedMidTermEpisode> result_;
    std::shared_future<void> release_signal_;
};

class RecordingMidTermFlushDecider final : public NiceMock<test::MockMidTermFlushDecider> {
  public:
    explicit RecordingMidTermFlushDecider(
        absl::StatusOr<MidTermFlushDecision> decision = MidTermFlushDecision{})
        : decision_(std::move(decision)) {
        ON_CALL(*this, Decide(_)).WillByDefault([this](const Conversation& conversation) {
            absl::StatusOr<MidTermFlushDecision> decision = MidTermFlushDecision{};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(conversation);
                decision = decision_;
            }
            return decision;
        });
    }

    [[nodiscard]] bool WaitForRequestCount(std::size_t expected_count) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requests_.size() >= expected_count) {
                    return true;
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    void SetDecision(absl::StatusOr<MidTermFlushDecision> decision) {
        std::lock_guard<std::mutex> lock(mutex_);
        decision_ = std::move(decision);
    }

    [[nodiscard]] std::vector<Conversation> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<Conversation> requests_;
    absl::StatusOr<MidTermFlushDecision> decision_;
};

class RecordingSleepCycleSemanticExtractor final : public SleepCycleSemanticExtractor {
  public:
    explicit RecordingSleepCycleSemanticExtractor(absl::StatusOr<SleepCycleSemanticExtractionResult>
                                                      result = SleepCycleSemanticExtractionResult{})
        : result_(std::move(result)) {}

    [[nodiscard]] absl::StatusOr<SleepCycleSemanticExtractionResult>
    Extract(const SleepCycleSemanticExtractionRequest& request) override {
        requests_.push_back(request);
        if (extract_fn_) {
            return extract_fn_(request);
        }
        return result_;
    }

    void SetResult(absl::StatusOr<SleepCycleSemanticExtractionResult> result) {
        result_ = std::move(result);
    }

    void SetExtractFn(std::function<absl::StatusOr<SleepCycleSemanticExtractionResult>(
                          const SleepCycleSemanticExtractionRequest&)>
                          extract_fn) {
        extract_fn_ = std::move(extract_fn);
    }

    [[nodiscard]] const std::vector<SleepCycleSemanticExtractionRequest>& requests() const {
        return requests_;
    }

  private:
    std::vector<SleepCycleSemanticExtractionRequest> requests_;
    absl::StatusOr<SleepCycleSemanticExtractionResult> result_;
    std::function<absl::StatusOr<SleepCycleSemanticExtractionResult>(
        const SleepCycleSemanticExtractionRequest&)>
        extract_fn_;
};

class MemoryOrchestratorTest : public ::testing::Test {
  protected:
    static Timestamp Ts(std::string_view text) {
        return json(text).get<Timestamp>();
    }

    static absl::StatusOr<MemoryOrchestrator> MakeHandler() {
        absl::StatusOr<WorkingMemory> memory = WorkingMemory::Create(WorkingMemoryInit{
            .system_prompt = "You are Isla.",
            .user_id = "user_001",
        });
        if (!memory.ok()) {
            return memory.status();
        }
        return MemoryOrchestrator("srv_test", std::move(*memory));
    }

    static absl::StatusOr<MemoryOrchestrator> MakeDefaultPromptHandler() {
        return MemoryOrchestrator::Create("srv_test", MemoryOrchestratorInit{
                                                          .user_id = "user_001",
                                                          .store = nullptr,
                                                      });
    }

    static absl::StatusOr<MemoryOrchestrator>
    MakeHandlerWithCompactor(const MidTermCompactorPtr& compactor, MemoryStorePtr store = nullptr,
                             const MidTermFlushDeciderPtr& decider = nullptr,
                             const SleepCycleSemanticExtractorPtr& semantic_extractor = nullptr,
                             std::size_t mid_term_flush_decider_interval_user_turns =
                                 kImmediateMidTermFlushDeciderIntervalUserTurns) {
        absl::StatusOr<WorkingMemory> memory = WorkingMemory::Create(WorkingMemoryInit{
            .system_prompt = "You are Isla.",
            .user_id = "user_001",
        });
        if (!memory.ok()) {
            return memory.status();
        }
        return MemoryOrchestrator("srv_test", std::move(*memory), std::move(store), decider,
                                  compactor, semantic_extractor,
                                  mid_term_flush_decider_interval_user_turns);
    }

    static absl::StatusOr<std::size_t> WaitForDrain(MemoryOrchestrator& orchestrator,
                                                    std::size_t expected_count) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            absl::StatusOr<std::size_t> drained = orchestrator.DrainCompletedMidTermCompactions();
            if (!drained.ok()) {
                return drained.status();
            }
            if (*drained == expected_count) {
                return drained;
            }
            std::this_thread::sleep_for(10ms);
        }
        return absl::DeadlineExceededError("timed out waiting for completed mid-term compactions");
    }

    static absl::Status WaitForDrainFailure(MemoryOrchestrator& orchestrator,
                                            absl::StatusCode expected_code) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            absl::StatusOr<std::size_t> drained = orchestrator.DrainCompletedMidTermCompactions();
            if (!drained.ok()) {
                if (drained.status().code() != expected_code) {
                    return drained.status();
                }
                return absl::OkStatus();
            }
            std::this_thread::sleep_for(10ms);
        }
        return absl::DeadlineExceededError("timed out waiting for mid-term compaction failure");
    }
};

} // namespace
} // namespace isla::server::memory
