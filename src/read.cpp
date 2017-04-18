/*!
 * @file read.cpp
 *
 * @brief Read class source file
 */

#include <assert.h>
#include <deque>
#include <algorithm>
#include <iostream>
#include <fstream>

#include "read.hpp"

namespace rala {

std::unique_ptr<Read> createRead(uint64_t id, const char* name, uint32_t name_length,
    const char* sequence, uint32_t sequence_length, const char* quality,
    uint32_t quality_length) {

    assert(sequence_length > 0);
    assert(sequence_length == quality_length);

    return std::unique_ptr<Read>(new Read(id, name, name_length, sequence,
        sequence_length, quality, quality_length));
}

Read::Read(uint64_t id, const char* name, uint32_t name_length, const char* sequence,
    uint32_t sequence_length)
        : id_(id), name_(name, name_length), sequence_(sequence, sequence_length),
        quality_(), rc_() {
}

Read::Read(uint64_t id, const char* name, uint32_t name_length, const char* sequence,
    uint32_t sequence_length, const char* quality, uint32_t quality_length)
        : id_(id), name_(name, name_length), sequence_(sequence, sequence_length),
        quality_(quality, quality_length), rc_() {
}

void Read::trim_sequence(uint32_t begin, uint32_t end) {
    sequence_ = sequence_.substr(begin, end - begin);
    quality_ = quality_.substr(begin, end - begin);
}

void Read::create_rc() {

    rc_.clear();
    for (int32_t i = sequence_.size() - 1; i >= 0; --i) {
        char c = sequence_[i];
        switch (c) {
            case 'A':
                c = 'T';
                break;
            case 'T':
                c = 'A';
                break;
            case 'C':
                c = 'G';
                break;
            case 'G':
                c = 'C';
                break;
            default:
                break;
        }
        rc_ += c;
    }
}

std::unique_ptr<ReadInfo> createReadInfo(uint64_t id, uint32_t read_length, std::vector<uint32_t>& mappings) {
    return mappings.empty() ? nullptr : std::unique_ptr<ReadInfo>(new ReadInfo(id, read_length, mappings));
}

ReadInfo::ReadInfo(uint64_t id, uint32_t read_length, std::vector<uint32_t>& mappings)
        : id_(id), begin_(0), end_(read_length), coverage_median_(0),
        coverage_graph_(read_length + 1, 0), coverage_hills_() {

    update_coverage_graph(mappings);
}

void ReadInfo::find_coverage_median() {

    std::vector<uint16_t> tmp(coverage_graph_);
    std::sort(tmp.begin(), tmp.end());
    coverage_median_ = tmp.size() % 2 == 1 ? tmp[tmp.size() / 2] : (tmp[tmp.size() / 2 - 1] + tmp[tmp.size() / 2]) / 2;
}

void ReadInfo::update_coverage_graph(std::vector<uint32_t>& mappings) {

    if (mappings.empty()) {
        return;
    }
    std::sort(mappings.begin(), mappings.end());

    uint16_t coverage = 0;
    uint32_t last_map = 0;
    for (const auto& map: mappings) {
        if (coverage > 0) {
            for (uint32_t i = last_map; i < (map >> 1); ++i) {
                coverage_graph_[i] += coverage;
            }
        }
        last_map = map >> 1;
        if (map & 1){
            --coverage;
        } else {
            ++coverage;
        }
    }
}

void ReadInfo::find_valid_region(uint32_t coverage) {

    if (coverage_graph_.empty()) {
        return;
    }

    uint32_t new_begin = 0, new_end = 0, current_begin = 0;
    bool found_begin = false;
    for (uint32_t i = begin_; i < end_; ++i) {
        if (!found_begin && coverage_graph_[i] >= coverage) {
            current_begin = i;
            found_begin = true;
        } else if (found_begin && coverage_graph_[i] < coverage) {
            if (i - current_begin > new_end - new_begin) {
                new_begin = current_begin;
                new_end = i;
            }
            found_begin = false;
        }
    }
    if (found_begin) {
        if (end_ - current_begin > new_end - new_begin) {
            new_begin = current_begin;
            new_end = end_;
        }
    }

    if (new_begin == new_end || new_end - new_begin < 500) {
        std::vector<uint16_t>().swap(coverage_graph_);
        begin_ = 0;
        end_ = 0;
    } else {
        for (uint32_t i = begin_; i < new_begin; ++i) {
            coverage_graph_[i] = 0;
        }
        for (uint32_t i = new_end; i < end_; ++i) {
            coverage_graph_[i] = 0;
        }
        begin_ = new_begin;
        end_ = new_end;
    }
}

void ReadInfo::find_coverage_pits(double slope_ratio, uint32_t min_slope_width,
    double slope_width_ratio) {

    if (coverage_graph_.empty()) {
        return;
    }

    std::deque<std::pair<int32_t, int32_t>> left_window, right_window;
    std::vector<int32_t> slopes;

    int32_t k = std::max(min_slope_width, uint32_t(slope_width_ratio * (end_ - begin_)));
    int32_t length = coverage_graph_.size();

    for (int32_t i = -1 * k + 2; i < length - 1; ++i) {
        if (i < length - k) {
            coverage_window_add(right_window, coverage_graph_[i + k], i + k);
        }
        coverage_window_update(right_window, i);

        if (i > 0) {
            coverage_window_add(left_window, coverage_graph_[i - 1], i - 1);
            coverage_window_update(left_window, i - 1 - k);

            int32_t current = coverage_graph_[i] * slope_ratio;
            if (left_window.front().second > current) {
                slopes.push_back(i << 1 | 0);
            }
            if (!right_window.empty() && right_window.front().second > current) {
                slopes.push_back(i << 1 | 1);
            }
        }
    }

    if (slopes.size() > 0) {
        std::sort(slopes.begin(), slopes.end());
        for (uint32_t i = 0; i < slopes.size() - 1; ++i) {
            if (!(slopes[i] & 1) && (slopes[i + 1] & 1) && (slopes[i + 1] >> 1) - (slopes[i] >> 1) < k) {
                // print_csv("graphs/c" + std::to_string(id_), coverage_median_);
                begin_ = 0;
                end_ = 0;
                std::vector<uint16_t>().swap(coverage_graph_);
                break;
            }
        }
    }
}

void ReadInfo::find_coverage_hills(double slope_ratio, uint32_t min_slope_width,
    double slope_width_ratio, double hill_width_ratio, uint32_t dataset_median) {

    if (coverage_graph_.empty()) {
        return;
    }

    std::deque<std::pair<int32_t, int32_t>> left_window, right_window;
    std::vector<int32_t> slopes;

    int32_t k = std::max(min_slope_width, uint32_t(slope_width_ratio * (end_ - begin_)));
    int32_t read_length = coverage_graph_.size();

    for (int32_t i = -1 * k + 2; i < read_length; ++i) {
        if (i < read_length - k) {
            coverage_window_add(right_window, coverage_graph_[i + k], i + k);
        }
        coverage_window_update(right_window, i);

        if (i > 0) {
            coverage_window_add(left_window, coverage_graph_[i - 1], i - 1);
            coverage_window_update(left_window, i - 1 - k);

            int32_t current = coverage_graph_[i] * slope_ratio;
            if (coverage_graph_[i - 1] > dataset_median && left_window.front().second > (int32_t) dataset_median && left_window.front().second > current) {
                slopes.push_back(i << 1 | 0);
            }
            if ((i == read_length - 1 || coverage_graph_[i + 1] > dataset_median) && !right_window.empty() && right_window.front().second > (int32_t) dataset_median && right_window.front().second > current) {
                slopes.push_back(i << 1 | 1);
            }
        }
    }

    if (slopes.size() > 2) {

        uint32_t ldownslope = 0, fdownslope = 0;
        bool found_fds = false;

        uint32_t fupslope = 0, lupslope = 0;
        bool found_fus = false;

        std::vector<std::pair<uint32_t, uint32_t>> slope_regions;

        uint32_t slope_width = k; // 2 * k;
        for (uint32_t s = 0; s < slopes.size(); ++s) {
            if (slopes[s] & 1) {
                if (found_fus) {
                    if ((slopes[s] >> 1) - fupslope > slope_width) {
                        slope_regions.emplace_back(fupslope << 1 | 1, lupslope);
                        fupslope = slopes[s] >> 1;
                        lupslope = fupslope;
                    } else {
                        lupslope = slopes[s] >> 1;
                    }
                } else {
                    found_fus = true;
                    fupslope = slopes[s] >> 1;
                    lupslope = fupslope;
                }
            } else {
                if (found_fds) {
                    if ((slopes[s] >> 1) - fdownslope > slope_width) {
                        slope_regions.emplace_back(fdownslope << 1 | 0, ldownslope);
                        fdownslope = slopes[s] >> 1;
                        ldownslope = fdownslope;
                    } else {
                        ldownslope = slopes[s] >> 1;
                    }
                } else {
                    found_fds = true;
                    fdownslope = slopes[s] >> 1;
                    ldownslope = fdownslope;
                }
            }
        }

        if (found_fus) slope_regions.emplace_back(fupslope << 1 | 1, lupslope);
        if (found_fds) slope_regions.emplace_back(fdownslope << 1 | 0, ldownslope);
        std::sort(slope_regions.begin(), slope_regions.end());

        // for (const auto& it: slope_regions) {
        //    fprintf(stderr, "%d (%d %d) \n", it.first & 1, it.first >> 1, it.second);
        // }

        // chimeric check - TODO: rerranging slopes does not work all the time
        auto rearrange_slopes = [&](uint32_t i, uint32_t j) -> void {
            uint32_t begin = std::max(slope_regions[i].first >> 1, slope_regions[j].first >> 1);
            uint32_t end = std::min(slope_regions[i].second, slope_regions[j].second);
            uint32_t min_left_id = begin, min_right_id = begin;
            for (uint32_t s = begin + 1; s < end; ++s) {
                if (coverage_graph_[s] < coverage_graph_[min_left_id]) {
                    min_left_id = s;
                }
                if (coverage_graph_[s] <= coverage_graph_[min_right_id]) {
                    min_right_id = s;
                }
            }
            ++min_left_id;
            --min_right_id;

            slope_regions[i].first = min_left_id << 1 | 0;
            slope_regions[i].second = min_left_id;
            slope_regions[j].first = min_right_id << 1 | 1;
            slope_regions[j].second = min_right_id;
        };

        for (uint32_t s = 0; s < slope_regions.size() - 1; ++s) {
            if ((slope_regions[s].first & 1) && !(slope_regions[s + 1].first & 1) &&
                slope_regions[s].second > (slope_regions[s + 1].first >> 1)) {
                rearrange_slopes(s, s + 1);
            }
        }

        for (uint32_t s = 0; s < slope_regions.size() - 1; ++s) {
            if (slope_regions[s].second > (slope_regions[s + 1].first >> 1)) {
                rearrange_slopes(s, s + 1);
            }
        }
        // fprintf(stderr, "Chimeric check\n");
        /* for (const auto& it: slope_regions) {
            fprintf(stderr, "%d (%d %d) \n", it.first & 1, it.first >> 1, it.second);
        }
        fprintf(stderr, "\n"); */

        // find hills
        auto check_hill = [&](uint32_t begin, uint32_t end, double median) -> bool {
            uint32_t max_border_value = std::max(coverage_graph_[begin], coverage_graph_[end]);
            uint32_t valid_bases = 0;
            for (uint32_t i = begin; i < end; ++i) {
                if (coverage_graph_[i] >= max_border_value) {
                    ++valid_bases;
                }
            }
            if (valid_bases > 0.8 * (end - begin)) {
                return true;
            }
            return false;
        };

        uint32_t max_width = coverage_graph_.size() * hill_width_ratio;
        for (uint32_t r = 0; r < slope_regions.size() - 1; ++r) {
            if ((slope_regions[r].first & 1) && !(slope_regions[r + 1].first & 1)) {
                if (slope_regions[r + 1].second - (slope_regions[r].first >> 1) < max_width) {

                    // fprintf(stderr, "%d: (%d,%d) - (%d,%d)\n", id, slope_regions[r].first >> 1, slope_regions[r].second,
                    //    slope_regions[r + 1].first >> 1, slope_regions[r + 1].second);

                    if (check_hill(slope_regions[r].second + 1, (slope_regions[r + 1].first >> 1) - 1, dataset_median) == false) {
                        continue;
                    }

                    if ((slope_regions[r].first >> 1) < 0.05 * (end_ - begin_) + begin_) {
                        // left hill
                        coverage_hills_.emplace_back(slope_regions[r].first >> 1, slope_regions[r + 1].second);
                    } else if (slope_regions[r + 1].second > 0.95 * (end_ - begin_) + begin_) {
                        // right hill
                        coverage_hills_.emplace_back(slope_regions[r].first >> 1, slope_regions[r + 1].second);
                    }
                }
            }
        }
    }
}

void ReadInfo::smooth_coverage_graph() {

    if (coverage_graph_.empty()) {
        return;
    }

    std::vector<uint16_t> new_coverage_graph(coverage_graph_.size(), 0);

    uint32_t k = 0.025 * coverage_graph_.size();
    std::deque<uint32_t> window(k, 0);
    window.insert(window.end(), coverage_graph_.begin(), coverage_graph_.begin() + k);

    uint32_t window_sum = 0;
    for (const auto& it: window) {
        window_sum += it;
    }

    for (uint32_t i = 0; i < coverage_graph_.size(); ++i) {
        if (i < coverage_graph_.size() - k) {
            window.emplace_back(coverage_graph_[i + k]);
            window_sum += window.back();
        } else {
            window.emplace_back(0);
        }
        new_coverage_graph[i] = window_sum / (2 * k + 1);

        window_sum -= window.front();
        window.pop_front();
    }

    coverage_graph_.swap(new_coverage_graph);
}

void ReadInfo::print_csv(std::string path, uint32_t dataset_median) const {

    if (coverage_graph_.empty()) {
        return;
    }

    std::vector<uint8_t> slope_graph(coverage_graph_.size(), 0);
    for (const auto& hill: coverage_hills_) {
        slope_graph[hill.first] = 2;
        slope_graph[hill.second] = 1;
    }

    std::ofstream out(path);
    for (uint32_t i = 0; i < coverage_graph_.size(); ++i) {
        out << i << " " << coverage_graph_[i] << " " << slope_graph[i] << " " << coverage_median_ << " " << dataset_median << std::endl;
    }
    out.close();
}

void ReadInfo::coverage_window_add(std::deque<std::pair<int32_t, int32_t>>& window, int32_t value, int32_t position) {
    while (!window.empty() && window.back().second <= value) {
        window.pop_back();
    }
    window.emplace_back(position, value);
}

void ReadInfo::coverage_window_update(std::deque<std::pair<int32_t, int32_t>>& window, int32_t position) {
    while (!window.empty() && window.front().first <= position) {
        window.pop_front();
    }
}

}
