#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include "threadpool.h"

extern const size_t BUFFER_SIZE;
extern const size_t OUTPUT_BUFFER_SIZE;
extern const int NUM_THREADS;

// Estructuras
struct HuffmanNode {
    unsigned char symbol;
    unsigned long long freq;
    HuffmanNode* left;
    HuffmanNode* right;
    
    HuffmanNode(unsigned char s, unsigned long long f);
    ~HuffmanNode();
};

struct CanonicalCode {
    unsigned char symbol;
    unsigned char length;
    unsigned int code;
};


struct ChunkMetadata {
    size_t offset;
    size_t compressedSize;
    size_t originalSize;
    unsigned char paddingBits;   
};

struct CompressionStats {
    std::string filename;
    size_t originalSize;
    size_t compressedSize;
    double compressionRatio;
    long long timeMs;
    bool success;
    std::string errorMsg;
    int threadId;
};

// Estructuras para procesamiento paralelo
enum class ChunkState {
    PENDING,
    PROCESSING,
    COMPLETED,
    ERROR
};

struct RawChunk {
    size_t index;
    std::vector<unsigned char> data;
    size_t offset;
    ChunkState state;
    
    RawChunk() : index(0), offset(0), state(ChunkState::PENDING) {}
    RawChunk(size_t idx, std::vector<unsigned char>&& d, size_t off) 
        : index(idx), data(std::move(d)), offset(off), state(ChunkState::PENDING) {}
};

struct CompressedChunk {
    size_t index;
    std::vector<unsigned char> data;
    ChunkMetadata metadata;
    ChunkState state;
    std::string errorMsg;
    
    CompressedChunk() : index(0), state(ChunkState::PENDING) {}
    CompressedChunk(size_t idx) : index(idx), state(ChunkState::PENDING) {}
};

struct DecompressedChunk {
    size_t index;
    std::vector<unsigned char> data;
    ChunkState state;
    std::string errorMsg;
    
    DecompressedChunk() : index(0), state(ChunkState::PENDING) {}
    DecompressedChunk(size_t idx) : index(idx), state(ChunkState::PENDING) {}
};

// Semáforo para control de flujo
class Semaphore {
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
    
public:
    explicit Semaphore(int count = 0) : count_(count) {}
    
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count_;
        cv_.notify_one();
    }
    
    void acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return count_ > 0; });
        --count_;
    }
    
    bool try_acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ > 0) {
            --count_;
            return true;
        }
        return false;
    }
};

// Colas thread-safe
template<typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_;
    
public:
    ThreadSafeQueue() : closed_(false) {}
    
    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_) {
            queue_.push(std::move(item));
            cv_.notify_one();
        }
    }
    
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return !queue_.empty() || closed_; });
        
        if (queue_.empty()) {
            return false;
        }
        
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }
    
    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
};


class FastBitWriter {
private:
    std::ofstream& file;
    std::vector<unsigned char> buffer;
    size_t bufferPos;
    unsigned long long bitBuffer;
    int bitsInBuffer;
    
public:
    FastBitWriter(std::ofstream& f);
    void writeBits(unsigned int code, int length);
    void flush();
    int getBitsInBuffer() const;
};

class FastBitReader {
private:
    std::ifstream& file;
    std::vector<unsigned char> buffer;
    size_t bufferPos;
    size_t bufferSize;
    unsigned long long bitBuffer;
    int bitsInBuffer;
    
    void fillBuffer();
    
public:
    FastBitReader(std::ifstream& f);
    int peekBits(int n);
    void consumeBits(int n);
    bool hasData() const;
};

struct FastDecoder {
    static const int LOOKUP_BITS = 12;
    
    struct Entry {
        unsigned char symbol;
        unsigned char length;
    };
    
    std::vector<Entry> lookupTable;
    std::unordered_map<unsigned long long, unsigned char> fallbackMap;
    int maxLength;
    
    FastDecoder(const std::vector<CanonicalCode>& codes);
};

// Funciones de chunks
size_t calculateOptimalChunkSize(size_t fileSize);
size_t calculateNumChunks(size_t fileSize, size_t chunkSize);

// Funciones principales
std::array<unsigned long long, 256> calculateFrequenciesParallel(const std::string& filename);
HuffmanNode* buildHuffmanTree(const std::array<unsigned long long, 256>& frequencies);
void generateCodeLengths(HuffmanNode* node, int depth, unsigned char* lengths);
std::vector<CanonicalCode> generateCanonicalCodes(const unsigned char* lengths);

// Funciones paralelas de compresión/descompresión
void compressParallel(const std::string& inputFile, const std::string& outputFile);
void decompressParallel(const std::string& inputFile, const std::string& outputFile);


void compress(const std::string& inputFile, const std::string& outputFile);
void decompress(const std::string& inputFile, const std::string& outputFile);

CompressionStats compressWithStats(const std::string& inputFile, const std::string& outputFile, ThreadPool* pool);
CompressionStats decompressWithStats(const std::string& inputFile, const std::string& outputFile, ThreadPool* pool);
void printStats(const std::vector<CompressionStats>& allStats, bool isDecompression = false);

#endif // HUFFMAN_H