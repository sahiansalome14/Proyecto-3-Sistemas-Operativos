#ifndef POSIX_UTILS_H
#define POSIX_UTILS_H

#include <string>
#include <vector>

bool posix_file_exists(const std::string& path);
bool posix_is_regular_file(const std::string& path);
bool posix_is_directory(const std::string& path);
size_t posix_file_size(const std::string& path);
bool posix_create_directories(const std::string& path);
std::string posix_get_extension(const std::string& path);
std::string posix_get_parent_path(const std::string& path);
std::string posix_get_filename(const std::string& path);
std::string posix_get_stem(const std::string& path);
std::string posix_join_path(const std::string& dir, const std::string& file);
std::string posix_normalize_path(const std::string& path);
std::string posix_relative_path(const std::string& path, const std::string& base);
void posix_recursive_directory_iterator(const std::string& dirPath, std::vector<std::string>& files, const std::string& basePath = "");

#endif // POSIX_UTILS_H