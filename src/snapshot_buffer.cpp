// Writer-thread implementation of the bounded repeat-on-segment snapshot.
#include "ros2_bag/snapshot_buffer.hpp"

#include <algorithm>

namespace ros2_bag {

SnapshotBuffer::SnapshotBuffer(std::size_t max_total_bytes) : max_total_bytes_(max_total_bytes) {}

void SnapshotBuffer::erase(Entries::iterator entry) noexcept {
    const auto usage = topic_usage_.find(entry->key);
    if (usage != topic_usage_.end()) {
        usage->second.used_bytes -= entry->message.payload_bytes;
        const auto item =
            std::find(usage->second.entries.begin(), usage->second.entries.end(), entry);
        if (item != usage->second.entries.end()) {
            usage->second.entries.erase(item);
        }
        if (usage->second.entries.empty()) {
            topic_usage_.erase(usage);
        }
    }
    used_bytes_ -= entry->message.payload_bytes;
    entries_.erase(entry);
}

void SnapshotBuffer::add(const RecordedMessage& message) {
    if (!message.topic_info || !message.serialized_message ||
        !message.topic_info->repeat_policy.enabled ||
        message.topic_info->repeat_policy.max_messages == 0 || message.payload_bytes == 0 ||
        message.payload_bytes > message.topic_info->repeat_policy.max_bytes ||
        message.payload_bytes > max_total_bytes_) {
        return;
    }
    const TopicKey key(message.topic_id, message.topic_epoch);
    const RepeatPolicy& policy = message.topic_info->repeat_policy;
    auto usage = topic_usage_.find(key);
    while (usage != topic_usage_.end() &&
           (usage->second.entries.size() >= policy.max_messages ||
            usage->second.used_bytes + message.payload_bytes > policy.max_bytes)) {
        erase(usage->second.entries.front());
        usage = topic_usage_.find(key);
    }
    while (used_bytes_ + message.payload_bytes > max_total_bytes_ && !entries_.empty()) {
        erase(entries_.begin());
    }
    entries_.push_back({message, key});
    const Entries::iterator added = std::prev(entries_.end());
    TopicUsage& added_usage = topic_usage_[key];
    added_usage.used_bytes += message.payload_bytes;
    added_usage.entries.push_back(added);
    used_bytes_ += message.payload_bytes;
}

void SnapshotBuffer::remove_topic(TopicId topic_id, std::uint64_t topic_epoch) {
    const TopicKey key(topic_id, topic_epoch);
    auto usage = topic_usage_.find(key);
    while (usage != topic_usage_.end() && !usage->second.entries.empty()) {
        erase(usage->second.entries.front());
        usage = topic_usage_.find(key);
    }
}

void SnapshotBuffer::retain_active_topics(const std::vector<TopicSnapshot>& topics) {
    std::map<TopicKey, bool> active;
    for (const TopicSnapshot& topic : topics) {
        if (topic.state == TopicRuntimeState::kActive || topic.state == TopicRuntimeState::kArmed) {
            active.insert(std::make_pair(TopicKey(topic.topic_info.id, topic.topic_epoch), true));
        }
    }
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        if (active.count(entry->key) == 0U) {
            const auto current = entry++;
            erase(current);
        } else {
            ++entry;
        }
    }
}

bool SnapshotBuffer::replay(const std::function<bool(const RecordedMessage&)>& write) const {
    for (const Entry& entry : entries_) {
        RecordedMessage replay = entry.message;
        replay.is_replayed = true;
        if (!write(replay)) {
            return false;
        }
    }
    return true;
}

void SnapshotBuffer::clear() noexcept {
    topic_usage_.clear();
    entries_.clear();
    used_bytes_ = 0;
}

std::size_t SnapshotBuffer::used_bytes() const noexcept { return used_bytes_; }

std::size_t SnapshotBuffer::message_count() const noexcept { return entries_.size(); }

}  // namespace ros2_bag
