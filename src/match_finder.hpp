// Candidate positions for matches, grouped by their first three bytes.
//
// The first two bytes pick one of 65536 binary search trees; inside a tree the
// third byte is the key. Every tree node keeps the positions sharing those
// three bytes in a list, newest first.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace flz::detail {

class MatchFinder {
public:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    MatchFinder() { clear(); }

    // Room for `positions` entries, so the position store does not grow by doubling.
    void reserve(std::size_t positions) { entries_.reserve(positions); }

    // Forgets every node and position.
    void clear()
    {
        roots_.assign(kRoots, kNone);
        nodes_.clear();
        free_node_ = kNone;
        entries_.clear();
        free_entry_ = kNone;
    }

    // Link slot (a root or a child pointer) holding the node for the three bytes
    // at `s`, or nullptr when there is none.
    std::uint32_t* find(const std::uint8_t* s)
    {
        std::uint32_t* slot = &roots_[root_index(s)];
        while (*slot != kNone) {
            Node& n = nodes_[*slot];
            if (s[2] == n.key)
                return slot;
            slot = s[2] < n.key ? &n.left : &n.right;
        }
        return nullptr;
    }

    // Adds `position` (whose three bytes start at `s`) as the newest entry. When
    // the list already holds `max_entries`, its oldest entry is reused.
    void insert(const std::uint8_t* s, std::uint32_t position, int max_entries)
    {
        // Link slots point into nodes_, so make sure creating a node below
        // cannot reallocate it.
        if (free_node_ == kNone && nodes_.size() == nodes_.capacity())
            nodes_.reserve(std::max<std::size_t>(1024, nodes_.capacity() * 2));

        std::uint32_t* slot = &roots_[root_index(s)];
        while (*slot != kNone) {
            Node& n = nodes_[*slot];
            if (s[2] == n.key)
                break;
            slot = s[2] < n.key ? &n.left : &n.right;
        }
        if (*slot == kNone)
            *slot = new_node(s[2]);

        Node& n = nodes_[*slot];
        std::uint32_t e;
        if (n.count >= max_entries) {
            e = n.last;
            unlink(n, e);
        } else {
            e = new_entry();
        }
        entries_[e].position = position;
        push_front(n, e);
    }

    // Visits the node's positions newest first. `keep(position, visited_before)`
    // runs for each; returning false drops the position. A node left empty is
    // removed from its tree.
    template <class Visit>
    void scan(std::uint32_t* slot, Visit&& keep)
    {
        Node& n = nodes_[*slot];
        int visited = 0;
        for (std::uint32_t e = n.first; e != kNone;) {
            const std::uint32_t next = entries_[e].next;
            if (!keep(entries_[e].position, visited)) {
                unlink(n, e);
                free_entry(e);
            }
            ++visited;
            e = next;
        }
        if (n.count == 0)
            erase(slot);
    }

    // Drops every position below `min_position` from the node in `slot`,
    // removing the node when it empties.
    void prune(std::uint32_t* slot, std::int64_t min_position)
    {
        Node& n = nodes_[*slot];
        for (std::uint32_t e = n.last; e != kNone;) {
            const std::uint32_t prev = entries_[e].prev;
            if (entries_[e].position < min_position) {
                unlink(n, e);
                free_entry(e);
            }
            e = prev;
        }
        if (n.count == 0)
            erase(slot);
    }

private:
    static constexpr std::size_t kRoots = 1u << 16;

    struct Node {
        std::uint32_t left, right;
        std::uint32_t first, last;  // newest and oldest entry
        int count;
        std::uint8_t key;
    };
    struct Entry {
        std::uint32_t position;
        std::uint32_t prev, next;  // towards newer / older entries
    };

    static std::size_t root_index(const std::uint8_t* s) { return s[0] | std::size_t{s[1]} << 8; }

    std::uint32_t new_node(std::uint8_t key)
    {
        std::uint32_t i;
        if (free_node_ != kNone) {
            i = free_node_;
            free_node_ = nodes_[i].left;
        } else {
            i = static_cast<std::uint32_t>(nodes_.size());
            nodes_.emplace_back();
        }
        nodes_[i] = Node{kNone, kNone, kNone, kNone, 0, key};
        return i;
    }

    std::uint32_t new_entry()
    {
        if (free_entry_ != kNone) {
            const std::uint32_t i = free_entry_;
            free_entry_ = entries_[i].next;
            return i;
        }
        entries_.emplace_back();
        return static_cast<std::uint32_t>(entries_.size() - 1);
    }

    void free_entry(std::uint32_t e)
    {
        entries_[e].next = free_entry_;
        free_entry_ = e;
    }

    void push_front(Node& n, std::uint32_t e)
    {
        entries_[e].prev = kNone;
        entries_[e].next = n.first;
        if (n.first != kNone)
            entries_[n.first].prev = e;
        else
            n.last = e;
        n.first = e;
        ++n.count;
    }

    void unlink(Node& n, std::uint32_t e)
    {
        const Entry& x = entries_[e];
        (x.prev != kNone ? entries_[x.prev].next : n.first) = x.next;
        (x.next != kNone ? entries_[x.next].prev : n.last) = x.prev;
        --n.count;
    }

    // Removes the (empty) node in `slot` from its search tree.
    void erase(std::uint32_t* slot)
    {
        const std::uint32_t i = *slot;
        Node& n = nodes_[i];
        assert(n.count == 0);
        std::uint32_t removed = i;
        if (n.left != kNone && n.right != kNone) {
            // Move the in-order predecessor's contents here and unlink it instead.
            std::uint32_t* pred = &n.left;
            while (nodes_[*pred].right != kNone)
                pred = &nodes_[*pred].right;
            removed = *pred;
            const Node& p = nodes_[removed];
            n.key = p.key;
            n.first = p.first;
            n.last = p.last;
            n.count = p.count;
            *pred = p.left;
        } else {
            *slot = n.left != kNone ? n.left : n.right;
        }
        nodes_[removed].left = free_node_;
        free_node_ = removed;
    }

    std::vector<std::uint32_t> roots_;
    std::vector<Node> nodes_;
    std::uint32_t free_node_ = kNone;
    std::vector<Entry> entries_;
    std::uint32_t free_entry_ = kNone;
};

}  // namespace flz::detail
