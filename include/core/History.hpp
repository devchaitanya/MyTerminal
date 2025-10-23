#pragma once
#include <string>
#include <deque>
#include <vector>

namespace myterm {

class History {
public:
    explicit History(size_t cap = 10000) : cap_(cap) {}
    void add(const std::string& cmd);
    const std::deque<std::string>& data() const { return h_; }
    void clear();
    // search: exact (most recent) else longest substring (>2)
    int search(const std::string& term) const;
    // Persistence
    void loadFromFile(const std::string& path);
    void appendToFile(const std::string& path, const std::string& cmd) const;
    void saveToFile(const std::string& path) const;
    // Best matches by longest common substring length (most recent first), min length > 2
    std::vector<std::string> bestSubstringMatches(const std::string& term) const;
private:
    size_t cap_;
    std::deque<std::string> h_;
};

} // namespace myterm
