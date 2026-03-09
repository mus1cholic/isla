#include "isla/server/memory/working_memory_utils.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "isla/server/memory/working_memory.hpp"

namespace isla::server::memory {
namespace {

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

template <typename Entry>
void EraseCacheEntry(std::vector<Entry>& entries, std::string_view entity_id) {
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [entity_id](const Entry& entry) { return entry.entity_id == entity_id; }),
        entries.end());
}

template <typename Entry>
void UpsertCacheEntry(std::vector<Entry>& entries, std::string entity_id, std::string text) {
    const auto it = std::find_if(entries.begin(), entries.end(), [&entity_id](const Entry& entry) {
        return entry.entity_id == entity_id;
    });
    if (it != entries.end()) {
        it->text = std::move(text);
        return;
    }
    entries.push_back(Entry{ .entity_id = std::move(entity_id), .text = std::move(text) });
}

void AppendLine(std::string& output, std::string_view line) {
    output.append(line);
    output.push_back('\n');
}

void AppendEscapedPromptText(std::string& output, std::string_view text) {
    output.append(EscapePromptText(text));
}

void AppendEpisodeLine(std::string& output, const Episode& episode) {
    output.append("- [");
    AppendEscapedPromptText(output, episode.episode_id);
    output.append(" | ");
    output.append(FormatTimestamp(episode.created_at));
    output.append(" | salience: ");
    output.append(std::to_string(episode.salience));
    if (IsExpandableEpisode(episode)) {
        output.append(" | expandable");
    }
    output.append("] ");
    AppendEscapedPromptText(output, episode.tier2_summary);
    output.push_back('\n');
}

void AppendMessageLine(std::string& output, const Message& message) {
    output.append("- [");
    switch (message.role) {
    case MessageRole::User:
        output.append("user");
        break;
    case MessageRole::Assistant:
        output.append("assistant");
        break;
    }
    output.append(" | ");
    output.append(FormatTimestamp(message.create_time));
    output.append("] ");
    AppendEscapedPromptText(output, message.content);
    output.push_back('\n');
}

void AppendEpisodeStubLine(std::string& output, const EpisodeStub& episode_stub) {
    output.append("- [stub | ");
    output.append(FormatTimestamp(episode_stub.create_time));
    output.append("] ");
    AppendEscapedPromptText(output, episode_stub.content);
    output.push_back('\n');
}

} // namespace

void UpsertActiveModel(PersistentMemoryCache& cache, std::string entity_id, std::string text) {
    EraseCacheEntry(cache.familiar_labels, entity_id);
    UpsertCacheEntry(cache.active_models, std::move(entity_id), std::move(text));
}

void UpsertFamiliarLabel(PersistentMemoryCache& cache, std::string entity_id, std::string text) {
    EraseCacheEntry(cache.active_models, entity_id);
    UpsertCacheEntry(cache.familiar_labels, std::move(entity_id), std::move(text));
}

std::string EscapePromptText(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '\\':
            escaped.append("\\\\");
            break;
        case '\n':
            escaped.append("\\n");
            break;
        case '\r':
            escaped.append("\\r");
            break;
        case '\t':
            escaped.append("\\t");
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

absl::StatusOr<std::string> RenderWorkingMemoryPrompt(const WorkingMemoryState& working_memory) {
    std::string output;
    output.append(working_memory.system_prompt);
    if (!output.empty() && output.back() != '\n') {
        output.push_back('\n');
    }

    AppendLine(output, "<persistent_memory_cache>");
    AppendLine(output, "Active Models:");
    if (working_memory.persistent_memory_cache.active_models.empty()) {
        AppendLine(output, "- (none)");
    } else {
        for (const ActiveModel& model : working_memory.persistent_memory_cache.active_models) {
            output.append("- [");
            AppendEscapedPromptText(output, model.entity_id);
            output.append("] ");
            AppendEscapedPromptText(output, model.text);
            output.push_back('\n');
        }
    }
    AppendLine(output, "Familiar Labels:");
    if (working_memory.persistent_memory_cache.familiar_labels.empty()) {
        AppendLine(output, "- (none)");
    } else {
        for (const FamiliarLabel& label : working_memory.persistent_memory_cache.familiar_labels) {
            output.append("- [");
            AppendEscapedPromptText(output, label.entity_id);
            output.append("] ");
            AppendEscapedPromptText(output, label.text);
            output.push_back('\n');
        }
    }

    AppendLine(output, "<mid_term_episodes>");
    if (working_memory.mid_term_episodes.empty()) {
        AppendLine(output, "- (none)");
    } else {
        for (const Episode& episode : working_memory.mid_term_episodes) {
            AppendEpisodeLine(output, episode);
        }
    }

    AppendLine(output, "<retrieved_memory>");
    if (working_memory.retrieved_memory.has_value()) {
        AppendEscapedPromptText(output, *working_memory.retrieved_memory);
        output.push_back('\n');
    } else {
        AppendLine(output, "(none)");
    }

    AppendLine(output, "<conversation>");
    if (working_memory.conversation.items.empty()) {
        AppendLine(output, "- (empty)");
    } else {
        for (const ConversationItem& item : working_memory.conversation.items) {
            switch (item.type) {
            case ConversationItemType::OngoingEpisode:
                if (!item.ongoing_episode.has_value()) {
                    return failed_precondition(
                        "conversation item type=ongoing_episode is missing payload");
                }
                for (const Message& message : item.ongoing_episode->messages) {
                    AppendMessageLine(output, message);
                }
                break;
            case ConversationItemType::EpisodeStub:
                if (!item.episode_stub.has_value()) {
                    return failed_precondition(
                        "conversation item type=episode_stub is missing payload");
                }
                AppendEpisodeStubLine(output, *item.episode_stub);
                break;
            }
        }
    }

    return output;
}

} // namespace isla::server::memory
