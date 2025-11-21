#include "posix_utils.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

bool posix_file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool posix_is_regular_file(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

bool posix_is_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

size_t posix_file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_size;
}

bool posix_create_directories(const std::string& path) {
    if (path.empty() || posix_is_directory(path)) return true;
    
    std::string dir = path;
    if (dir.back() == '/') dir.pop_back();
    
    size_t pos = 0;
    while ((pos = dir.find('/', pos + 1)) != std::string::npos) {
        std::string subdir = dir.substr(0, pos);
        if (!subdir.empty() && !posix_is_directory(subdir)) {
            if (mkdir(subdir.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    
    if (!posix_is_directory(dir)) {
        if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    
    return true;
}

std::string posix_get_extension(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return path.substr(dot);
    }
    return "";
}

std::string posix_get_parent_path(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string posix_get_filename(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string posix_get_stem(const std::string& path) {
    std::string filename = posix_get_filename(path);
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) return filename;
    return filename.substr(0, dot);
}

std::string posix_join_path(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (file.empty()) return dir;
    if (dir.back() == '/') return dir + file;
    return dir + "/" + file;
}

std::string posix_normalize_path(const std::string& path) {
    std::string result;
    bool lastWasSlash = false;
    
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!lastWasSlash) {
                result += '/';
                lastWasSlash = true;
            }
        } else {
            result += c;
            lastWasSlash = false;
        }
    }
    
    if (result.length() > 1 && result.back() == '/') {
        result.pop_back();
    }
    
    return result;
}

std::string posix_relative_path(const std::string& path, const std::string& base) {
    std::string normPath = posix_normalize_path(path);
    std::string normBase = posix_normalize_path(base);
    
    if (normPath.find(normBase) == 0) {
        size_t start = normBase.length();
        if (start < normPath.length() && normPath[start] == '/') {
            start++;
        }
        return normPath.substr(start);
    }
    
    return normPath;
}

void posix_recursive_directory_iterator(
    const std::string& dirPath,
    std::vector<std::string>& files,
    const std::string& basePath
) {
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        
        if (name == "." || name == "..") continue;
        
        std::string fullPath = posix_join_path(dirPath, name);
        
        if (posix_is_directory(fullPath)) {
            posix_recursive_directory_iterator(fullPath, files, basePath);
        } else if (posix_is_regular_file(fullPath)) {
            files.push_back(fullPath);
        }
    }
    
    closedir(dir);
}