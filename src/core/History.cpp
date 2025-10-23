#include "core/History.hpp"
#include <algorithm>
#include <fstream>
#include <unordered_set>

namespace myterm {

void History::add(const std::string& cmd) {
    if (cmd.empty()) return;
    if (!h_.empty() && h_.back()==cmd) return;
    if (h_.size() == cap_) h_.pop_front();
    h_.push_back(cmd);
}

void History::clear() {
    h_.clear();
}

int History::search(const std::string& term) const {
    if (term.empty()) return -1;
    for (int i=(int)h_.size()-1;i>=0;--i) if (h_[i]==term) return i;
    if (term.size()<=2) return -1;
    // longest substring among equals; we return first found
    for (int i=0;i<(int)h_.size();++i) if (h_[i].find(term)!=std::string::npos) return i;
    return -1;
}

void History::loadFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    std::deque<std::string> tmp;
    while (std::getline(in, line)) {
        if (!line.empty()) tmp.push_back(line);
    }
    // keep only last cap_
    size_t start = (tmp.size() > cap_) ? (tmp.size() - cap_) : 0;
    for (size_t i = start; i < tmp.size(); ++i) add(tmp[i]);
}

void History::appendToFile(const std::string& path, const std::string& cmd) const {
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    out << cmd << '\n';
}

void History::saveToFile(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    for (const auto& s : h_) out << s << '\n';
}

static int lcs_substr_len(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0;
    const size_t n=a.size(), m=b.size();
    int best=0;
    std::vector<int> prev(m+1,0), cur(m+1,0);
    for (size_t i=1;i<=n;++i) {
        for (size_t j=1;j<=m;++j) {
            if (a[i-1]==b[j-1]) cur[j]=prev[j-1]+1; else cur[j]=0;
            if (cur[j]>best) best=cur[j];
        }
        std::swap(prev, cur);
        // reset cur for next row
        std::fill(cur.begin(), cur.end(), 0);
    }
    return best;
}

std::vector<std::string> History::bestSubstringMatches(const std::string& term) const {
    std::vector<std::string> results;
    if (term.empty()) return results;
    int bestLen = 0;
    std::unordered_set<std::string> seen; // dedupe while keeping most-recent-first order
    for (int i=(int)h_.size()-1; i>=0; --i) {
        const std::string& cmd = h_[i];
        if (seen.find(cmd) != seen.end()) continue;
        // Safe skip: if the maximum possible match for this candidate is
        // strictly less than current best, it cannot improve or tie.
        if ((int)std::min(cmd.size(), term.size()) < bestLen) continue;
        int l = lcs_substr_len(cmd, term);
        if (l > bestLen) {
            bestLen = l;
            results.clear();
            if (l > 2) { results.push_back(cmd); seen.insert(cmd); }
        } else if (l == bestLen && l > 2) {
            results.push_back(cmd);
            seen.insert(cmd);
        }
    }
    return results;
}

} // namespace myterm
