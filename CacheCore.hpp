#ifndef CACHE_CORE_HPP
#define CACHE_CORE_HPP

#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
#include <string>

enum class ReplacementPolicy {
    LRU,
    FIFO,
    RANDOM
};

struct CacheLine {
    bool valid = false;
    bool dirty = false;
    uint64_t tag = 0;
    uint64_t last_access = 0;
    uint64_t insertion_time = 0;
};

struct CacheSet {
    std::vector<CacheLine> lines;
};

struct StepResult {
    bool hit = false;
    bool evicted = false;
    bool dirty_eviction = false;
    uint32_t set_index = 0;
    uint64_t tag = 0;
    int way = -1;
    int evicted_way = -1;
    uint64_t address = 0;
    char op = 'R';
};

class CacheCore {
public:
    CacheCore() : rng(std::random_device{}()) {}

    static bool isPowerOfTwo(uint32_t val) {
        return (val > 0) && ((val & (val - 1)) == 0);
    }

    static uint32_t log2_uint(uint32_t val) {
        uint32_t bits = 0;
        while (val >>= 1) ++bits;
        return bits;
    }

    bool configure(uint32_t total_cache_size, uint32_t block_size, uint32_t associativity, ReplacementPolicy policy) {
        // Enforce strict power-of-2 validation for Cache Size, Block Size, and Associativity
        if (!isPowerOfTwo(total_cache_size) || !isPowerOfTwo(block_size) || !isPowerOfTwo(associativity)) {
            return false;
        }

        if (block_size > total_cache_size) return false;
        uint32_t total_lines = total_cache_size / block_size;
        if (associativity > total_lines) return false;

        m_cache_size = total_cache_size;
        m_block_size = block_size;
        m_associativity = associativity;
        m_policy = policy;

        m_num_sets = total_lines / m_associativity;
        if (!isPowerOfTwo(m_num_sets)) return false;

        // Bitwise mask offsets using exact integer log2
        m_offset_bits = log2_uint(m_block_size);
        m_index_bits = log2_uint(m_num_sets);
        m_index_mask = m_num_sets - 1;

        reset();
        m_configured = true;
        return true;
    }

    void reset() {
        m_sets.clear();
        m_sets.resize(m_num_sets);
        for (auto& set : m_sets) {
            set.lines.resize(m_associativity);
        }
        m_hits = 0;
        m_misses = 0;
        m_evictions = 0;
        m_access_counter = 0;
    }

    StepResult access(char op, uint64_t address) {
        StepResult res;
        res.op = (op == 'w' || op == 'W') ? 'W' : 'R';
        res.address = address;

        if (!m_configured || m_num_sets == 0) return res;

        m_access_counter++;

        // Bitwise extraction: Shift out offset bits, mask set index, shift remaining for tag
        res.set_index = (address >> m_offset_bits) & m_index_mask;
        res.tag = address >> (m_offset_bits + m_index_bits);

        CacheSet& set = m_sets[res.set_index];

        // 1. Check for Cache Hit
        for (uint32_t i = 0; i < m_associativity; ++i) {
            if (set.lines[i].valid && set.lines[i].tag == res.tag) {
                res.hit = true;
                res.way = static_cast<int>(i);
                set.lines[i].last_access = m_access_counter;
                if (res.op == 'W') {
                    set.lines[i].dirty = true;
                }
                m_hits++;
                return res;
            }
        }

        // Cache Miss handling
        res.hit = false;
        m_misses++;

        // 2. Check for empty invalid line in set
        for (uint32_t i = 0; i < m_associativity; ++i) {
            if (!set.lines[i].valid) {
                set.lines[i].valid = true;
                set.lines[i].tag = res.tag;
                set.lines[i].last_access = m_access_counter;
                set.lines[i].insertion_time = m_access_counter;
                set.lines[i].dirty = (res.op == 'W');
                res.way = static_cast<int>(i);
                res.evicted = false;
                return res;
            }
        }

        // 3. Set is full: Execute eviction replacement policy
        m_evictions++;
        res.evicted = true;
        int victim_way = selectVictim(set);

        res.dirty_eviction = set.lines[victim_way].dirty;

        set.lines[victim_way].tag = res.tag;
        set.lines[victim_way].last_access = m_access_counter;
        set.lines[victim_way].insertion_time = m_access_counter;
        set.lines[victim_way].dirty = (res.op == 'W');

        res.way = victim_way;
        res.evicted_way = victim_way;
        return res;
    }

    // Accessors
    bool isConfigured() const { return m_configured; }
    uint32_t getCacheSize() const { return m_cache_size; }
    uint32_t getBlockSize() const { return m_block_size; }
    uint32_t getAssociativity() const { return m_associativity; }
    uint32_t getNumSets() const { return m_num_sets; }
    uint64_t getHits() const { return m_hits; }
    uint64_t getMisses() const { return m_misses; }
    uint64_t getEvictions() const { return m_evictions; }
    ReplacementPolicy getPolicy() const { return m_policy; }
    const std::vector<CacheSet>& getSets() const { return m_sets; }

private:
    int selectVictim(const CacheSet& set) {
        int victim = 0;
        if (m_policy == ReplacementPolicy::LRU) {
            // Select line with smallest last_access timestamp
            uint64_t min_access = set.lines[0].last_access;
            for (uint32_t i = 1; i < m_associativity; ++i) {
                if (set.lines[i].last_access < min_access) {
                    min_access = set.lines[i].last_access;
                    victim = i;
                }
            }
        } else if (m_policy == ReplacementPolicy::FIFO) {
            // Select line with smallest insertion_time timestamp
            uint64_t min_insert = set.lines[0].insertion_time;
            for (uint32_t i = 1; i < m_associativity; ++i) {
                if (set.lines[i].insertion_time < min_insert) {
                    min_insert = set.lines[i].insertion_time;
                    victim = i;
                }
            }
        } else if (m_policy == ReplacementPolicy::RANDOM) {
            std::uniform_int_distribution<uint32_t> dist(0, m_associativity - 1);
            victim = static_cast<int>(dist(rng));
        }
        return victim;
    }

    bool m_configured = false;
    uint32_t m_cache_size = 0;
    uint32_t m_block_size = 0;
    uint32_t m_associativity = 0;
    uint32_t m_num_sets = 0;
    ReplacementPolicy m_policy = ReplacementPolicy::LRU;

    uint32_t m_offset_bits = 0;
    uint32_t m_index_bits = 0;
    uint64_t m_index_mask = 0;

    uint64_t m_hits = 0;
    uint64_t m_misses = 0;
    uint64_t m_evictions = 0;
    uint64_t m_access_counter = 0;

    std::vector<CacheSet> m_sets;
    std::mt19937 rng;
};

#endif // CACHE_CORE_HPP
