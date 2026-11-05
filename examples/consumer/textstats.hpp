#pragma once
#include <map>
#include <string>
#include <vector>

std::vector<std::string> tokenize(const std::string& text);
std::map<std::string, int> word_counts(const std::string& text);
std::string most_common(const std::string& text);
