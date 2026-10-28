#include "ros2_bag/recorder/snapshot.hpp"

#include <algorithm>
#include <stdexcept>

namespace ros2_bag::recorder {

Snapshot::Snapshot(SnapshotConfig config) : config_(config) {
    if (config_.max_messages_per_topic == 0 || config_.max_bytes_per_topic == 0 ||
        config_.max_total_bytes == 0) {
        throw std::invalid_argument("Snapshot budgets must be positive");
    }
}

void Snapshot::push_written_live(
    const RecordedMessage& message,
    std::chrono::steady_clock::time_point now) {
    if (message.replayed) {
        return;
    }
    auto& topic = entries_[message.topic_id];
    topic.push_back(Entry{next_sequence_++, message, now});
    total_bytes_ += message.payload_bytes;
    enforce_topic(topic, now);
    enforce_total();
}

std::vector<RecordedMessage> Snapshot::replay_for(std::uint64_t generation) const {
    std::vector<Entry> ordered;
    for (const auto& topic : entries_) {
        for (const auto& entry : topic.second) {
            if (entry.message.generation == generation) {
                ordered.push_back(entry);
            }
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const Entry& left, const Entry& right) {
        return left.sequence < right.sequence;
    });
    std::vector<RecordedMessage> replay;
    replay.reserve(ordered.size());
    for (const auto& entry : ordered) {
        RecordedMessage message = entry.message;
        message.replayed = true;
        replay.push_back(std::move(message));
    }
    return replay;
}

void Snapshot::erase_topic(TopicId topic_id, std::uint64_t topic_epoch) {
    const auto found = entries_.find(topic_id);
    if (found == entries_.end()) {
        return;
    }
    auto& topic = found->second;
    for (auto entry = topic.begin(); entry != topic.end();) {
        if (entry->message.topic_epoch == topic_epoch) {
            erase_entry(topic, entry);
            entry = topic.begin();
        } else {
            ++entry;
        }
    }
    if (topic.empty()) {
        entries_.erase(found);
    }
}

void Snapshot::enforce_topic(TopicEntries& entries, std::chrono::steady_clock::time_point now) {
    std::size_t bytes = 0;
    for (const auto& entry : entries) {
        bytes += entry.message.payload_bytes;
    }
    while (!entries.empty() &&
           (entries.size() > config_.max_messages_per_topic ||
            bytes > config_.max_bytes_per_topic ||
            (config_.max_age && now - entries.front().inserted_at > *config_.max_age))) {
        bytes -= entries.front().message.payload_bytes;
        total_bytes_ -= entries.front().message.payload_bytes;
        entries.pop_front();
    }
}

void Snapshot::enforce_total() {
    while (total_bytes_ > config_.max_total_bytes) {
        auto oldest_topic = entries_.end();
        for (auto topic = entries_.begin(); topic != entries_.end(); ++topic) {
            if (topic->second.empty()) {
                continue;
            }
            if (oldest_topic == entries_.end() ||
                topic->second.front().sequence < oldest_topic->second.front().sequence) {
                oldest_topic = topic;
            }
        }
        if (oldest_topic == entries_.end()) {
            return;
        }
        total_bytes_ -= oldest_topic->second.front().message.payload_bytes;
        oldest_topic->second.pop_front();
        if (oldest_topic->second.empty()) {
            entries_.erase(oldest_topic);
        }
    }
}

void Snapshot::erase_entry(TopicEntries& entries, TopicEntries::iterator entry) {
    total_bytes_ -= entry->message.payload_bytes;
    entries.erase(entry);
}

}  // namespace ros2_bag::recorder
