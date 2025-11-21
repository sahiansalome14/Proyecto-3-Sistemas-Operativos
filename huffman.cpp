#include "huffman.h"
#include "posix_utils.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sstream>

const size_t BUFFER_SIZE = 8 * 1024 * 1024;
const size_t OUTPUT_BUFFER_SIZE = 4 * 1024 * 1024;
const int NUM_THREADS = std::thread::hardware_concurrency();

std::mutex consoleMutex;

struct ChunkMetadataSimple {
    size_t offset;
    size_t compressedSize;
    size_t originalSize;
    unsigned char paddingBits;
};


size_t calculateOptimalChunkSize(size_t fileSize) {
    if (fileSize < 100ULL * 1024 * 1024) return fileSize;
    if (fileSize < 1024ULL * 1024 * 1024) return 128ULL * 1024 * 1024;
    if (fileSize < 4096ULL * 1024 * 1024) return 256ULL * 1024 * 1024;
    return 512ULL * 1024 * 1024;
}

size_t calculateNumChunks(size_t fileSize, size_t chunkSize) {
    return (fileSize + chunkSize - 1) / chunkSize;
}

HuffmanNode::HuffmanNode(unsigned char s, unsigned long long f) 
    : symbol(s), freq(f), left(nullptr), right(nullptr) {}

HuffmanNode::~HuffmanNode() {
    delete left;
    delete right;
}

struct CompareNode {
    bool operator()(HuffmanNode* a, HuffmanNode* b) {
        return a->freq > b->freq;
    }
};

// FastBitWriter
FastBitWriter::FastBitWriter(std::ofstream& f) 
    : file(f), buffer(OUTPUT_BUFFER_SIZE), bufferPos(0), bitBuffer(0), bitsInBuffer(0) {}

void FastBitWriter::writeBits(unsigned int code, int length) {
    bitBuffer = (bitBuffer << length) | code;
    bitsInBuffer += length;
    
    while (bitsInBuffer >= 8) {
        bitsInBuffer -= 8;
        buffer[bufferPos++] = (bitBuffer >> bitsInBuffer) & 0xFF;
        
        if (bufferPos == OUTPUT_BUFFER_SIZE) {
            file.write(reinterpret_cast<char*>(buffer.data()), OUTPUT_BUFFER_SIZE);
            bufferPos = 0;
        }
    }
}

void FastBitWriter::flush() {
    if (bitsInBuffer > 0) {
        buffer[bufferPos++] = (bitBuffer << (8 - bitsInBuffer)) & 0xFF;
    }
    
    if (bufferPos > 0) {
        file.write(reinterpret_cast<char*>(buffer.data()), bufferPos);
    }
}

int FastBitWriter::getBitsInBuffer() const { 
    return bitsInBuffer; 
}

// FastBitReader
FastBitReader::FastBitReader(std::ifstream& f)
    : file(f), buffer(BUFFER_SIZE), bufferPos(0), bufferSize(0),
      bitBuffer(0), bitsInBuffer(0) {
    if (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), BUFFER_SIZE);
        bufferSize = file.gcount();
        bufferPos = 0;
    }
}

void FastBitReader::fillBuffer() {
    while (bitsInBuffer < 56) {
        if (bufferPos >= bufferSize) {
            if (!file) break;
            file.read(reinterpret_cast<char*>(buffer.data()), BUFFER_SIZE);
            bufferSize = file.gcount();
            bufferPos = 0;
            if (bufferSize == 0) break;
        }
        bitBuffer = (bitBuffer << 8) | buffer[bufferPos++];
        bitsInBuffer += 8;
    }
}

int FastBitReader::peekBits(int n) {
    if (n <= 0) return 0;
    while (bitsInBuffer < n) {
        if (bufferPos >= bufferSize) {
            if (!file) break;
            file.read(reinterpret_cast<char*>(buffer.data()), BUFFER_SIZE);
            bufferSize = file.gcount();
            bufferPos = 0;
            if (bufferSize == 0) break;
        }
        bitBuffer = (bitBuffer << 8) | buffer[bufferPos++];
        bitsInBuffer += 8;
    }

    if (bitsInBuffer == 0) return -1;

    if (bitsInBuffer < n) {
        unsigned long long available = bitBuffer & ((1ULL << bitsInBuffer) - 1ULL);
        unsigned int shift = n - bitsInBuffer;
        unsigned long long val = (available << shift) & ((1ULL << n) - 1ULL);
        return static_cast<int>(val);
    }

    unsigned long long val = (bitBuffer >> (bitsInBuffer - n)) & ((1ULL << n) - 1ULL);
    return static_cast<int>(val);
}

void FastBitReader::consumeBits(int n) {
    if (n <= 0) return;
    if (n > bitsInBuffer) n = bitsInBuffer;
    bitsInBuffer -= n;
    if (bitsInBuffer == 0) {
        bitBuffer = 0;
    } else {
        bitBuffer &= ((1ULL << bitsInBuffer) - 1ULL);
    }
}

bool FastBitReader::hasData() const {
    return bitsInBuffer > 0 || file.good();
}

// FastDecoder
FastDecoder::FastDecoder(const std::vector<CanonicalCode>& codes) : maxLength(0) {
    lookupTable.resize(1 << LOOKUP_BITS, {0, 0});
    
    for (const auto& cc : codes) {
        maxLength = std::max(maxLength, static_cast<int>(cc.length));
        
        if (cc.length <= LOOKUP_BITS) {
            unsigned int baseCode = cc.code << (LOOKUP_BITS - cc.length);
            unsigned int range = 1 << (LOOKUP_BITS - cc.length);
            
            for (unsigned int i = 0; i < range; i++) {
                lookupTable[baseCode + i] = {cc.symbol, cc.length};
            }
        } else {
            unsigned long long key = (static_cast<unsigned long long>(cc.code) << 8) | cc.length;
            fallbackMap[key] = cc.symbol;
        }
    }
}


struct FrequencyChunk {
    unsigned long long freqs[256];
    
    FrequencyChunk() {
        std::memset(freqs, 0, sizeof(freqs));
    }
};

void calculateFrequenciesChunk(const unsigned char* data, size_t size, FrequencyChunk& result) {
    size_t i = 0;
    size_t limit16 = size - (size % 16);
    
    for (; i < limit16; i += 16) {
        result.freqs[data[i]]++;
        result.freqs[data[i + 1]]++;
        result.freqs[data[i + 2]]++;
        result.freqs[data[i + 3]]++;
        result.freqs[data[i + 4]]++;
        result.freqs[data[i + 5]]++;
        result.freqs[data[i + 6]]++;
        result.freqs[data[i + 7]]++;
        result.freqs[data[i + 8]]++;
        result.freqs[data[i + 9]]++;
        result.freqs[data[i + 10]]++;
        result.freqs[data[i + 11]]++;
        result.freqs[data[i + 12]]++;
        result.freqs[data[i + 13]]++;
        result.freqs[data[i + 14]]++;
        result.freqs[data[i + 15]]++;
    }
    
    for (; i < size; i++) {
        result.freqs[data[i]]++;
    }
}

std::array<unsigned long long, 256> calculateFrequenciesParallel(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("No se puede abrir el archivo: " + filename);
    }
    
    size_t fileSize = file.tellg();
    file.seekg(0);
    
    std::array<unsigned long long, 256> frequencies;
    frequencies.fill(0);
    
    if (fileSize < 16 * 1024 * 1024) {
        std::vector<unsigned char> buffer(fileSize);
        file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
        
        FrequencyChunk chunk;
        calculateFrequenciesChunk(buffer.data(), fileSize, chunk);
        std::copy(chunk.freqs, chunk.freqs + 256, frequencies.begin());
        
        file.close();
        return frequencies;
    }
    
    file.close();
    
    int input_fd = open(filename.c_str(), O_RDONLY);
    if (input_fd < 0) {
        throw std::runtime_error("No se puede abrir el archivo: " + filename);
    }
    
    const size_t chunkSize = 16 * 1024 * 1024;
    const size_t numChunks = (fileSize + chunkSize - 1) / chunkSize;
    
    std::vector<FrequencyChunk> results(numChunks);
    
    std::queue<std::pair<size_t, std::vector<unsigned char>>> readQueue;
    std::mutex queueMutex;
    std::condition_variable cv;
    std::atomic<bool> readingDone(false);
    std::atomic<bool> error_occurred(false);
    
    std::thread readerThread([&]() {
        for (size_t i = 0; i < numChunks && !error_occurred.load(); i++) {
            size_t offset = i * chunkSize;
            size_t readSize = std::min(chunkSize, fileSize - offset);
            std::vector<unsigned char> buffer(readSize);
            
            ssize_t bytesRead = pread(input_fd, buffer.data(), readSize, offset);
            
            if (bytesRead != (ssize_t)readSize) {
                error_occurred = true;
                break;
            }
            
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                readQueue.push({i, std::move(buffer)});
            }
            cv.notify_one();
        }
        
        readingDone = true;
        cv.notify_all();
    });
    
    int numWorkers = std::min(NUM_THREADS, (int)numChunks);
    numWorkers = std::max(2, numWorkers);
    
    std::vector<std::thread> workers;
    
    for (int t = 0; t < numWorkers; t++) {
        workers.emplace_back([&]() {
            while (!error_occurred.load()) {
                std::pair<size_t, std::vector<unsigned char>> task;
                
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    cv.wait(lock, [&]() { 
                        return !readQueue.empty() || readingDone.load() || error_occurred.load(); 
                    });
                    
                    if (error_occurred.load()) break;
                    if (readQueue.empty() && readingDone.load()) break;
                    if (readQueue.empty()) continue;
                    
                    task = std::move(readQueue.front());
                    readQueue.pop();
                }
                
                calculateFrequenciesChunk(
                    task.second.data(), 
                    task.second.size(), 
                    results[task.first]
                );
            }
        });
    }
    
    readerThread.join();
    for (auto& w : workers) {
        w.join();
    }
    
    close(input_fd);
    
    if (error_occurred.load()) {
        throw std::runtime_error("Error durante el cálculo de frecuencias");
    }
    
    for (const auto& chunk : results) {
        for (int i = 0; i < 256; i++) {
            frequencies[i] += chunk.freqs[i];
        }
    }
    
    return frequencies;
}



HuffmanNode* buildHuffmanTree(const std::array<unsigned long long, 256>& frequencies) {
    std::priority_queue<HuffmanNode*, std::vector<HuffmanNode*>, CompareNode> pq;
    
    for (int i = 0; i < 256; i++) {
        if (frequencies[i] > 0) {
            pq.push(new HuffmanNode(i, frequencies[i]));
        }
    }
    
    if (pq.empty()) {
        HuffmanNode* root = new HuffmanNode(0, 0);
        root->left = new HuffmanNode(0, 0);
        return root;
    }
    
    if (pq.size() == 1) {
        HuffmanNode* single = pq.top();
        HuffmanNode* root = new HuffmanNode(0, single->freq);
        root->left = single;
        return root;
    }
    
    while (pq.size() > 1) {
        HuffmanNode* left = pq.top(); pq.pop();
        HuffmanNode* right = pq.top(); pq.pop();
        
        HuffmanNode* parent = new HuffmanNode(0, left->freq + right->freq);
        parent->left = left;
        parent->right = right;
        pq.push(parent);
    }
    
    return pq.top();
}

void generateCodeLengths(HuffmanNode* node, int depth, unsigned char* lengths) {
    if (!node) return;
    
    if (!node->left && !node->right) {
        lengths[node->symbol] = (depth == 0) ? 1 : depth;
        return;
    }
    
    generateCodeLengths(node->left, depth + 1, lengths);
    generateCodeLengths(node->right, depth + 1, lengths);
}

std::vector<CanonicalCode> generateCanonicalCodes(const unsigned char* lengths) {
    std::vector<CanonicalCode> codes;
    codes.reserve(256);
    
    for (int i = 0; i < 256; i++) {
        if (lengths[i] > 0) {
            codes.push_back({static_cast<unsigned char>(i), lengths[i], 0});
        }
    }
    
    std::sort(codes.begin(), codes.end(), [](const CanonicalCode& a, const CanonicalCode& b) {
        if (a.length != b.length) return a.length < b.length;
        return a.symbol < b.symbol;
    });
    
    unsigned int code = 0;
    int prevLength = 0;
    
    for (auto& cc : codes) {
        if (cc.length > prevLength) {
            code <<= (cc.length - prevLength);
            prevLength = cc.length;
        }
        cc.code = code;
        code++;
    }
    
    return codes;
}

CompressedChunk compressChunkWorker(const RawChunk& rawChunk) {
    CompressedChunk result(rawChunk.index);
    
    try {
        if (rawChunk.data.empty()) {
            result.state = ChunkState::ERROR;
            result.errorMsg = "Chunk vacío";
            return result;
        }
        
        // Calcular frecuencias del chunk
        FrequencyChunk freqChunk;
        calculateFrequenciesChunk(rawChunk.data.data(), rawChunk.data.size(), freqChunk);
        
        std::array<unsigned long long, 256> frequencies;
        std::copy(freqChunk.freqs, freqChunk.freqs + 256, frequencies.begin());
        
        // Construir árbol Huffman
        HuffmanNode* root = buildHuffmanTree(frequencies);
        
        unsigned char codeLengths[256] = {0};
        generateCodeLengths(root, 0, codeLengths);
        
        auto canonicalCodes = generateCanonicalCodes(codeLengths);
        
        if (canonicalCodes.empty()) {
            canonicalCodes.push_back({0, 1, 0});
        }
        
        // Crear tabla de búsqueda rápida
        CanonicalCode codeTable[256];
        std::memset(codeTable, 0, sizeof(codeTable));
        for (const auto& cc : canonicalCodes) {
            codeTable[cc.symbol] = cc;
        }
        
        // Escritura optimizada de header
        std::vector<unsigned char> compressedData;
        compressedData.reserve(rawChunk.data.size() / 2);
        
        // Escribir número de símbolos (2 bytes)
        uint16_t numSymbols = canonicalCodes.size();
        compressedData.push_back(numSymbols & 0xFF);
        compressedData.push_back((numSymbols >> 8) & 0xFF);
        
       
        for (const auto& cc : canonicalCodes) {
            compressedData.push_back(cc.symbol);
            compressedData.push_back(cc.length);
        }
        
        // Comprimir datos
        unsigned long long bitBuffer = 0;
        int bitsInBuffer = 0;
        
        for (size_t i = 0; i < rawChunk.data.size(); i++) {
            const auto& code = codeTable[rawChunk.data[i]];
            
            if (code.length == 0) {
                continue;
            }
            
            bitBuffer = (bitBuffer << code.length) | code.code;
            bitsInBuffer += code.length;
            
            while (bitsInBuffer >= 8) {
                bitsInBuffer -= 8;
                compressedData.push_back((bitBuffer >> bitsInBuffer) & 0xFF);
            }
        }
        
        // Cálculo correcto de paddingBits 
        unsigned char paddingBits = 0;
        if (bitsInBuffer > 0) {
            // Hay bits restantes que no completan un byte
            paddingBits = 8 - bitsInBuffer;  // Cuántos bits de padding necesitamos
            compressedData.push_back((bitBuffer << paddingBits) & 0xFF);
        }
        // Si bitsInBuffer == 0, no agregamos byte extra y paddingBits = 0
        
        result.data = std::move(compressedData);
        result.metadata.originalSize = rawChunk.data.size();
        result.metadata.compressedSize = result.data.size();
        result.metadata.paddingBits = paddingBits;
        result.state = ChunkState::COMPLETED;
        
        delete root;
        
    } catch (const std::exception& e) {
        result.state = ChunkState::ERROR;
        result.errorMsg = e.what();
    }
    
    return result;
}

void compressParallel(const std::string& inputFile, const std::string& outputFile) {
    std::ifstream inFile(inputFile, std::ios::binary | std::ios::ate);
    if (!inFile) {
        throw std::runtime_error("No se puede abrir el archivo de entrada");
    }
    
    size_t fileSize = inFile.tellg();
    inFile.close();
    
    // Para archivos pequeños, comprimir como un solo chunk
    if (fileSize < 1024 * 1024) {
        std::ifstream file(inputFile, std::ios::binary);
        std::vector<unsigned char> data(fileSize);
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
        file.close();
        
        RawChunk single(0, std::move(data), 0);
        auto compressed = compressChunkWorker(single);
        
        if (compressed.state == ChunkState::ERROR) {
            throw std::runtime_error(compressed.errorMsg);
        }
        
        std::ofstream out(outputFile, std::ios::binary);
        out.write("HUF2", 4);
        uint32_t numChunks = 1;
        out.write(reinterpret_cast<char*>(&numChunks), sizeof(numChunks));
        
        uint64_t metadataOffset = 16 + compressed.data.size();
        out.write(reinterpret_cast<char*>(&metadataOffset), sizeof(metadataOffset));
        
        out.write(reinterpret_cast<char*>(compressed.data.data()), compressed.data.size());
        
        uint64_t offset = 16;
        uint64_t compSize = compressed.data.size();
        uint64_t origSize = compressed.metadata.originalSize;
        
        out.write(reinterpret_cast<char*>(&offset), sizeof(offset));
        out.write(reinterpret_cast<char*>(&compSize), sizeof(compSize));
        out.write(reinterpret_cast<char*>(&origSize), sizeof(origSize));
        out.write(reinterpret_cast<char*>(&compressed.metadata.paddingBits), 1);
        
        out.close();
        return;
    }
    
    // Para archivos grandes, usar pipeline paralelo
    size_t chunkSize = calculateOptimalChunkSize(fileSize);
    size_t numChunks = calculateNumChunks(fileSize, chunkSize);
    
    int total_cores = std::thread::hardware_concurrency();
    int reserved_cores = 2;
    int available_workers = std::max(1, total_cores - reserved_cores);
    
    Semaphore input_slots(available_workers * 2);
    Semaphore output_slots(available_workers);
    
    ThreadSafeQueue<RawChunk> inputQueue;
    ThreadSafeQueue<CompressedChunk> outputQueue;
    
    std::atomic<bool> error_occurred(false);
    std::string error_message;
    std::mutex error_mutex;
    
    // Thread lector
    std::thread readerThread([&]() {
        try {
            int input_fd = open(inputFile.c_str(), O_RDONLY);
            if (input_fd < 0) {
                throw std::runtime_error("No se puede abrir archivo de entrada");
            }
            
            for (size_t i = 0; i < numChunks && !error_occurred.load(); i++) {
                size_t offset = i * chunkSize;
                size_t currentSize = std::min(chunkSize, fileSize - offset);
                
                std::vector<unsigned char> buffer(currentSize);
                ssize_t bytesRead = pread(input_fd, buffer.data(), currentSize, offset);
                
                if (bytesRead != (ssize_t)currentSize) {
                    throw std::runtime_error("Error leyendo chunk");
                }
                
                input_slots.acquire();
                inputQueue.push(RawChunk(i, std::move(buffer), offset));
            }
            
            close(input_fd);
            inputQueue.close();
            
        } catch (const std::exception& e) {
            error_occurred = true;
            std::lock_guard<std::mutex> lock(error_mutex);
            error_message = e.what();
            inputQueue.close();
        }
    });
    
    // Worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < available_workers; i++) {
        workers.emplace_back([&]() {
            while (!error_occurred.load()) {
                RawChunk rawChunk;
                if (!inputQueue.pop(rawChunk)) {
                    break;
                }
                
                auto compressed = compressChunkWorker(rawChunk);
                
                if (compressed.state == ChunkState::ERROR) {
                    error_occurred = true;
                    std::lock_guard<std::mutex> lock(error_mutex);
                    error_message = compressed.errorMsg;
                    outputQueue.close();
                    break;
                }
                
                output_slots.acquire();
                outputQueue.push(std::move(compressed));
                input_slots.release();
            }
        });
    }
    
    // Thread escritor
    std::vector<ChunkMetadataSimple> chunksMetadata(numChunks);
    
    std::thread writerThread([&]() {
        try {
            std::ofstream outFile(outputFile, std::ios::binary);
            if (!outFile) {
                throw std::runtime_error("No se puede crear archivo de salida");
            }
            
            outFile.write("HUF2", 4);
            uint32_t numChunks32 = static_cast<uint32_t>(numChunks);
            outFile.write(reinterpret_cast<char*>(&numChunks32), sizeof(numChunks32));
            
            size_t metadataOffsetPos = outFile.tellp();
            uint64_t metadataOffset = 0;
            outFile.write(reinterpret_cast<char*>(&metadataOffset), sizeof(metadataOffset));
            
            std::vector<CompressedChunk> chunkBuffer(numChunks);
            size_t nextToWrite = 0;
            size_t received = 0;
            
            while (received < numChunks && !error_occurred.load()) {
                CompressedChunk chunk;
                if (!outputQueue.pop(chunk)) {
                    break;
                }
                
                chunkBuffer[chunk.index] = std::move(chunk);
                received++;
                
                while (nextToWrite < numChunks && 
                       chunkBuffer[nextToWrite].state == ChunkState::COMPLETED) {
                    
                    auto& c = chunkBuffer[nextToWrite];
                    
                    chunksMetadata[nextToWrite].offset = outFile.tellp();
                    outFile.write(reinterpret_cast<char*>(c.data.data()), c.data.size());
                    
                    chunksMetadata[nextToWrite].compressedSize = c.data.size();
                    chunksMetadata[nextToWrite].originalSize = c.metadata.originalSize;
                    chunksMetadata[nextToWrite].paddingBits = c.metadata.paddingBits;
                    
                    nextToWrite++;
                    output_slots.release();
                }
            }
            
            metadataOffset = outFile.tellp();
            
            for (const auto& meta : chunksMetadata) {
                uint64_t offset64 = meta.offset;
                uint64_t compSize64 = meta.compressedSize;
                uint64_t origSize64 = meta.originalSize;
                
                outFile.write(reinterpret_cast<char*>(&offset64), sizeof(offset64));
                outFile.write(reinterpret_cast<char*>(&compSize64), sizeof(compSize64));
                outFile.write(reinterpret_cast<char*>(&origSize64), sizeof(origSize64));
                outFile.write(reinterpret_cast<const char*>(&meta.paddingBits), 1);
            }
            
            outFile.seekp(metadataOffsetPos);
            outFile.write(reinterpret_cast<char*>(&metadataOffset), sizeof(metadataOffset));
            
            outFile.close();
            
        } catch (const std::exception& e) {
            error_occurred = true;
            std::lock_guard<std::mutex> lock(error_mutex);
            error_message = e.what();
        }
    });
    
    readerThread.join();
    for (auto& w : workers) {
        w.join();
    }
    outputQueue.close();
    writerThread.join();
    
    if (error_occurred.load()) {
        throw std::runtime_error("Error en compresión paralela: " + error_message);
    }
}


DecompressedChunk decompressChunkWorker(const std::vector<unsigned char>& compressedData,
                                        size_t originalSize,
                                        unsigned char paddingBits,
                                        size_t index) {
    DecompressedChunk result(index);
    
    try {
        size_t pos = 0;
        
        if (compressedData.size() < 2) {
            throw std::runtime_error("Datos comprimidos inválidos");
        }
        
        // Leer número de símbolos
        uint16_t numSymbols = compressedData[pos] | (compressedData[pos + 1] << 8);
        pos += 2;
        
        if (pos + numSymbols * 2 > compressedData.size()) {
            throw std::runtime_error("Tabla de códigos truncada");
        }
        
        std::vector<CanonicalCode> codes;
        codes.reserve(numSymbols);
        
        for (int i = 0; i < numSymbols; i++) {
            unsigned char symbol = compressedData[pos++];
            unsigned char length = compressedData[pos++];
            codes.push_back({symbol, length, 0});
        }
        
        // Reconstruir códigos canónicos
        std::sort(codes.begin(), codes.end(), [](const CanonicalCode& a, const CanonicalCode& b) {
            if (a.length != b.length) return a.length < b.length;
            return a.symbol < b.symbol;
        });
        
        unsigned int code = 0;
        int prevLength = 0;
        for (auto& cc : codes) {
            if (cc.length > prevLength) {
                code <<= (cc.length - prevLength);
                prevLength = cc.length;
            }
            cc.code = code;
            code++;
        }
        
        FastDecoder decoder(codes);
        
        // Descomprimir datos
        std::vector<unsigned char> output;
        output.reserve(originalSize);
        
        unsigned long long bitBuffer = 0;
        int bitsInBuffer = 0;
        
     
        size_t dataStartPos = pos;
        size_t totalDataBytes = compressedData.size() - dataStartPos;

        size_t totalValidBits = (totalDataBytes * 8) - paddingBits;
        size_t bitsProcessed = 0;
        
        // Llenar buffer inicial
        while (bitsInBuffer < 56 && pos < compressedData.size()) {
            bitBuffer = (bitBuffer << 8) | compressedData[pos++];
            bitsInBuffer += 8;
        }
        
        // Decodificar símbolos
        while (output.size() < originalSize) {
            // Verificar si tenemos suficientes bits válidos para continuar
            if (bitsProcessed >= totalValidBits) {
                break;  
            }
            
            // Intentar decodificar con lookup table
            if (bitsInBuffer >= FastDecoder::LOOKUP_BITS) {
                int lookupBits = (bitBuffer >> (bitsInBuffer - FastDecoder::LOOKUP_BITS)) & 
                                ((1 << FastDecoder::LOOKUP_BITS) - 1);
                
                auto& entry = decoder.lookupTable[lookupBits];
                
                if (entry.length > 0 && entry.length <= bitsInBuffer) {
                    // Verificar que no excedamos los bits válidos
                    if (bitsProcessed + entry.length <= totalValidBits) {
                        output.push_back(entry.symbol);
                        bitsInBuffer -= entry.length;
                        bitsProcessed += entry.length;
                        bitBuffer &= ((1ULL << bitsInBuffer) - 1);
                    } else {
                        break;  // No hay suficientes bits válidos
                    }
                } else {
                    // Buscar en fallback map para códigos más largos
                    bool found = false;
                    for (int len = FastDecoder::LOOKUP_BITS + 1; 
                         len <= decoder.maxLength && len <= bitsInBuffer; len++) {
                        
                        // Verificar que no excedamos los bits válidos
                        if (bitsProcessed + len > totalValidBits) {
                            break;
                        }
                        
                        int longerBits = (bitBuffer >> (bitsInBuffer - len)) & ((1 << len) - 1);
                        unsigned long long key = (static_cast<unsigned long long>(longerBits) << 8) | len;
                        auto it = decoder.fallbackMap.find(key);
                        
                        if (it != decoder.fallbackMap.end()) {
                            output.push_back(it->second);
                            bitsInBuffer -= len;
                            bitsProcessed += len;
                            bitBuffer &= ((1ULL << bitsInBuffer) - 1);
                            found = true;
                            break;
                        }
                    }
                    
                    if (!found) {
                        // No se encontró código válido
                        throw std::runtime_error("Código Huffman inválido encontrado");
                    }
                }
            } else if (bitsInBuffer > 0) {
                // Intentar decodificar con los bits que quedan
                bool found = false;
                for (int len = 1; len <= bitsInBuffer && len <= decoder.maxLength; len++) {
                    if (bitsProcessed + len > totalValidBits) {
                        break;
                    }
                    
                    int bits = (bitBuffer >> (bitsInBuffer - len)) & ((1 << len) - 1);
                    
                    // Buscar en lookup table si es posible
                    if (len <= FastDecoder::LOOKUP_BITS) {
                        int paddedBits = bits << (FastDecoder::LOOKUP_BITS - len);
                        auto& entry = decoder.lookupTable[paddedBits];
                        if (entry.length == len) {
                            output.push_back(entry.symbol);
                            bitsInBuffer -= len;
                            bitsProcessed += len;
                            bitBuffer &= ((1ULL << bitsInBuffer) - 1);
                            found = true;
                            break;
                        }
                    } else {
                        // Buscar en fallback map
                        unsigned long long key = (static_cast<unsigned long long>(bits) << 8) | len;
                        auto it = decoder.fallbackMap.find(key);
                        if (it != decoder.fallbackMap.end()) {
                            output.push_back(it->second);
                            bitsInBuffer -= len;
                            bitsProcessed += len;
                            bitBuffer &= ((1ULL << bitsInBuffer) - 1);
                            found = true;
                            break;
                        }
                    }
                }
                
                if (!found) {
                    break;  // No hay más códigos válidos
                }
            } else {
                break;  // No quedan bits en el buffer
            }
            
            // Recargar buffer si es necesario y hay más datos
            while (bitsInBuffer < 56 && pos < compressedData.size()) {
                bitBuffer = (bitBuffer << 8) | compressedData[pos++];
                bitsInBuffer += 8;
            }
        }
        
        // Verificar que se descomprimió el tamaño correcto
        if (output.size() != originalSize) {
            throw std::runtime_error("Tamaño descomprimido no coincide: esperado " + 
                                   std::to_string(originalSize) + ", obtenido " + 
                                   std::to_string(output.size()));
        }
        
        result.data = std::move(output);
        result.state = ChunkState::COMPLETED;
        
    } catch (const std::exception& e) {
        result.state = ChunkState::ERROR;
        result.errorMsg = e.what();
    }
    
    return result;
}

void decompressParallel(const std::string& inputFile, const std::string& outputFile) {
    std::ifstream inFile(inputFile, std::ios::binary);
    if (!inFile) {
        throw std::runtime_error("No se puede abrir el archivo comprimido");
    }
    
    // Leer header
    char header[4];
    inFile.read(header, 4);
    
    if (std::strncmp(header, "HUF2", 4) != 0) {
        inFile.close();
        // Fallback a descompresión secuencial para formato antiguo
        decompress(inputFile, outputFile);
        return;
    }
    
    uint32_t numChunks;
    inFile.read(reinterpret_cast<char*>(&numChunks), sizeof(numChunks));
    
    uint64_t metadataOffset;
    inFile.read(reinterpret_cast<char*>(&metadataOffset), sizeof(metadataOffset));
    
    // Leer metadata table
    inFile.seekg(metadataOffset);
    
    std::vector<ChunkMetadataSimple> chunksMetadata(numChunks);
    
    for (size_t i = 0; i < numChunks; i++) {
        ChunkMetadataSimple& meta = chunksMetadata[i];
        
        uint64_t offset64, compSize64, origSize64;
        inFile.read(reinterpret_cast<char*>(&offset64), sizeof(offset64));
        inFile.read(reinterpret_cast<char*>(&compSize64), sizeof(compSize64));
        inFile.read(reinterpret_cast<char*>(&origSize64), sizeof(origSize64));
        inFile.read(reinterpret_cast<char*>(&meta.paddingBits), 1);
        
        meta.offset = offset64;
        meta.compressedSize = compSize64;
        meta.originalSize = origSize64;
    }
    
    inFile.close();
    
    // Configurar cores
    int total_cores = std::thread::hardware_concurrency();
    int reserved_cores = 2;
    int available_workers = std::max(1, total_cores - reserved_cores);
    
    // Semáforos
    Semaphore input_slots(available_workers * 2);
    Semaphore output_slots(available_workers);
    
    // Colas
    ThreadSafeQueue<std::pair<size_t, std::vector<unsigned char>>> inputQueue;
    ThreadSafeQueue<DecompressedChunk> outputQueue;
    
    std::atomic<bool> error_occurred(false);
    std::string error_message;
    std::mutex error_mutex;
    
    // Thread lector
    std::thread readerThread([&]() {
        try {
            int input_fd = open(inputFile.c_str(), O_RDONLY);
            if (input_fd < 0) {
                throw std::runtime_error("No se puede abrir archivo comprimido");
            }
            
            for (size_t i = 0; i < numChunks && !error_occurred.load(); i++) {
                const auto& meta = chunksMetadata[i];
                
                std::vector<unsigned char> compressedData(meta.compressedSize);
                ssize_t bytesRead = pread(input_fd, compressedData.data(), 
                                         meta.compressedSize, meta.offset);
                
                if (bytesRead != (ssize_t)meta.compressedSize) {
                    throw std::runtime_error("Error leyendo chunk comprimido");
                }
                
                input_slots.acquire();
                inputQueue.push({i, std::move(compressedData)});
            }
            
            close(input_fd);
            inputQueue.close();
            
        } catch (const std::exception& e) {
            error_occurred = true;
            std::lock_guard<std::mutex> lock(error_mutex);
            error_message = e.what();
            inputQueue.close();
        }
    });
    
    // Worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < available_workers; i++) {
        workers.emplace_back([&]() {
            while (!error_occurred.load()) {
                std::pair<size_t, std::vector<unsigned char>> task;
                if (!inputQueue.pop(task)) {
                    break;
                }
                
                size_t index = task.first;
                auto& compData = task.second;
                size_t originalSize = chunksMetadata[index].originalSize;
                unsigned char paddingBits = chunksMetadata[index].paddingBits;
                
                auto decompressed = decompressChunkWorker(compData, originalSize, paddingBits, index);
                
                if (decompressed.state == ChunkState::ERROR) {
                    error_occurred = true;
                    std::lock_guard<std::mutex> lock(error_mutex);
                    error_message = decompressed.errorMsg;
                    outputQueue.close();
                    break;
                }
                
                output_slots.acquire();
                outputQueue.push(std::move(decompressed));
                input_slots.release();
            }
        });
    }
    
    // Thread escritor
    std::thread writerThread([&]() {
        try {
            std::ofstream outFile(outputFile, std::ios::binary | std::ios::trunc);
            if (!outFile) {
                throw std::runtime_error("No se puede crear archivo de salida");
            }
            
            std::vector<DecompressedChunk> chunkBuffer(numChunks);
            size_t nextToWrite = 0;
            size_t received = 0;
            
            while (received < numChunks && !error_occurred.load()) {
                DecompressedChunk chunk;
                if (!outputQueue.pop(chunk)) {
                    break;
                }
                
                chunkBuffer[chunk.index] = std::move(chunk);
                received++;
                
                while (nextToWrite < numChunks && 
                       chunkBuffer[nextToWrite].state == ChunkState::COMPLETED) {
                    
                    auto& c = chunkBuffer[nextToWrite];
                    outFile.write(reinterpret_cast<char*>(c.data.data()), c.data.size());
                    
                    nextToWrite++;
                    output_slots.release();
                }
            }
            
            outFile.flush();
            outFile.close();
            
        } catch (const std::exception& e) {
            error_occurred = true;
            std::lock_guard<std::mutex> lock(error_mutex);
            error_message = e.what();
        }
    });
    
    readerThread.join();
    for (auto& w : workers) {
        w.join();
    }
    outputQueue.close();
    writerThread.join();
    
    if (error_occurred.load()) {
        throw std::runtime_error("Error en descompresión paralela: " + error_message);
    }
}

void compress(const std::string& inputFile, const std::string& outputFile) {
    compressParallel(inputFile, outputFile);
}

void decompress(const std::string& inputFile, const std::string& outputFile) {
    std::ifstream inFile(inputFile, std::ios::binary);
    if (!inFile) {
        throw std::runtime_error("No se puede abrir el archivo comprimido");
    }
    
    // Leer header
    char header[4];
    inFile.read(header, 4);
    
    bool isChunked = (std::strncmp(header, "HUF2", 4) == 0);
    
    if (!isChunked) {
        // Formato antiguo (HUF)
        if (std::strncmp(header, "HUF", 3) != 0) {
            throw std::runtime_error("Formato de archivo inválido");
        }
        
        // Descompresión antigua
        unsigned short numSymbols;
        inFile.read(reinterpret_cast<char*>(&numSymbols), sizeof(numSymbols));

        std::vector<CanonicalCode> codes;
        codes.reserve(numSymbols);
        for (int i = 0; i < numSymbols; i++) {
            unsigned char symbol, length;
            inFile.read(reinterpret_cast<char*>(&symbol), 1);
            inFile.read(reinterpret_cast<char*>(&length), 1);
            codes.push_back({symbol, length, 0});
        }

        std::sort(codes.begin(), codes.end(), [](const CanonicalCode& a, const CanonicalCode& b) {
            if (a.length != b.length) return a.length < b.length;
            return a.symbol < b.symbol;
        });

        unsigned int code = 0;
        int prevLength = 0;
        for (auto& cc : codes) {
            if (cc.length > prevLength) {
                code <<= (cc.length - prevLength);
                prevLength = cc.length;
            }
            cc.code = code;
            code++;
        }

        FastDecoder decoder(codes);

        unsigned long long originalSize;
        inFile.read(reinterpret_cast<char*>(&originalSize), sizeof(originalSize));

        std::streampos currentPos = inFile.tellg();
        inFile.seekg(0, std::ios::end);

        inFile.seekg(-1, std::ios::end);
        unsigned char paddingBits;
        inFile.read(reinterpret_cast<char*>(&paddingBits), 1);
        if (paddingBits == 8) paddingBits = 0;

        inFile.seekg(currentPos, std::ios::beg);

        std::ofstream outFile(outputFile, std::ios::binary | std::ios::trunc);
        if (!outFile) throw std::runtime_error("No se puede crear el archivo de salida");

        if (originalSize == 0) return;

        FastBitReader reader(inFile);
        std::vector<unsigned char> outputBuffer;
        outputBuffer.reserve(OUTPUT_BUFFER_SIZE);

        unsigned long long bytesWritten = 0;

        while (bytesWritten < originalSize) {
            int bits = reader.peekBits(FastDecoder::LOOKUP_BITS);
            if (bits < 0) break;

            auto& entry = decoder.lookupTable[bits];
            if (entry.length > 0) {
                outputBuffer.push_back(entry.symbol);
                reader.consumeBits(entry.length);
                bytesWritten++;
            } else {
                bool found = false;
                for (int len = FastDecoder::LOOKUP_BITS + 1; len <= decoder.maxLength; len++) {
                    int longerBits = reader.peekBits(len);
                    if (longerBits < 0) break;
                    unsigned long long key = (static_cast<unsigned long long>(longerBits) << 8) | len;
                    auto it = decoder.fallbackMap.find(key);
                    if (it != decoder.fallbackMap.end()) {
                        outputBuffer.push_back(it->second);
                        reader.consumeBits(len);
                        bytesWritten++;
                        found = true;
                        break;
                    }
                }
                if (!found) reader.consumeBits(1);
            }

            if (outputBuffer.size() >= OUTPUT_BUFFER_SIZE || bytesWritten == originalSize) {
                outFile.write(reinterpret_cast<char*>(outputBuffer.data()), outputBuffer.size());
                outputBuffer.clear();
            }
        }
        
        if (!outputBuffer.empty()) {
            outFile.write(reinterpret_cast<char*>(outputBuffer.data()), outputBuffer.size());
        }

        outFile.flush();
        outFile.close();
        inFile.close();
        return;
    }
    
    // Formato nuevo (HUF2) - usar versión paralela
    inFile.close();
    decompressParallel(inputFile, outputFile);
}



CompressionStats compressWithStats(const std::string& inputFile, const std::string& outputFile, ThreadPool* pool) {
    CompressionStats stats;
    stats.filename = inputFile;
    stats.success = false;
    stats.threadId = pool->getThreadId();
    
    {
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ... Comprimiendo: " 
                  << inputFile << std::endl;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    try {
        stats.originalSize = posix_file_size(inputFile);
        compressParallel(inputFile, outputFile);
        stats.compressedSize = posix_file_size(outputFile);
        stats.compressionRatio = (stats.originalSize > 0) ? (100.0 * stats.compressedSize / stats.originalSize) : 0.0;
        stats.success = true;
        
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        {
            std::lock_guard<std::mutex> lock(consoleMutex);
            std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ✓ Completado: " 
                      << inputFile 
                      << " (" << stats.originalSize << " → " << stats.compressedSize 
                      << " bytes, " << std::fixed << std::setprecision(1) << stats.compressionRatio 
                      << "%, " << stats.timeMs << " ms)" << std::endl;
        }
        
    } catch (const std::exception& e) {
        stats.errorMsg = e.what();
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ✗ Error: " 
                  << inputFile << " - " << e.what() << std::endl;
    }
    
    return stats;
}

CompressionStats decompressWithStats(const std::string& inputFile, const std::string& outputFile, ThreadPool* pool) {
    CompressionStats stats;
    stats.filename = inputFile;
    stats.success = false;
    stats.threadId = pool->getThreadId();
    
    {
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ... Descomprimiendo: " 
                  << inputFile << std::endl;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    try {
        stats.originalSize = posix_file_size(inputFile);
        decompressParallel(inputFile, outputFile);
        stats.compressedSize = posix_file_size(outputFile);
        stats.success = true;
        
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        {
            std::lock_guard<std::mutex> lock(consoleMutex);
            std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ✓ Completado: " 
                      << inputFile 
                      << " (" << stats.originalSize << " → " << stats.compressedSize 
                      << " bytes, " << stats.timeMs << " ms)" << std::endl;
        }
        
    } catch (const std::exception& e) {
        stats.errorMsg = e.what();
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ✗ Error: " 
                  << inputFile << " - " << e.what() << std::endl;
    }
    
    return stats;
}

void printStats(const std::vector<CompressionStats>& allStats, bool isDecompression) {
    size_t totalOriginal = 0, totalCompressed = 0, successes = 0;
    long long totalTime = 0;

    std::cout << "\n" << std::string(100, '=');
    if (isDecompression)
        std::cout << "\nRESUMEN DE DESCOMPRESIÓN\n";
    else
        std::cout << "\nRESUMEN DE COMPRESIÓN\n";
    std::cout << std::string(100, '=') << "\n\n";

    if (isDecompression) {
        std::cout << std::left << std::setw(40) << "Archivo"
                  << std::setw(15) << "Comprimido"
                  << std::setw(15) << "Descomprimido"
                  << std::setw(12) << "Tiempo (ms)"
                  << "Estado\n" << std::string(100, '-') << "\n";
    } else {
        std::cout << std::left << std::setw(40) << "Archivo"
                  << std::setw(15) << "Original"
                  << std::setw(15) << "Comprimido"
                  << std::setw(12) << "Ratio %"
                  << std::setw(12) << "Tiempo (ms)"
                  << "Estado\n" << std::string(100, '-') << "\n";
    }

    for (const auto& stat : allStats) {
        std::cout << std::left << std::setw(40) << stat.filename.substr(0, 39);
        if (stat.success) {
            if (isDecompression) {
                std::cout << std::setw(15) << stat.originalSize
                          << std::setw(15) << stat.compressedSize
                          << std::setw(12) << stat.timeMs
                          << "✓ OK\n";
            } else {
                std::cout << std::setw(15) << stat.originalSize
                          << std::setw(15) << stat.compressedSize
                          << std::setw(12) << std::fixed << std::setprecision(2)
                          << stat.compressionRatio
                          << std::setw(12) << stat.timeMs
                          << "✓ OK\n";
            }

            totalOriginal += stat.originalSize;
            totalCompressed += stat.compressedSize;
            totalTime += stat.timeMs;
            successes++;
        } else {
            std::cout << std::setw(15) << "-" << std::setw(15) << "-"
                      << std::setw(12) << stat.timeMs
                      << "✗ ERROR: " << stat.errorMsg << "\n";
        }
    }

    std::cout << std::string(100, '-') << "\n" << std::left << std::setw(40) << "TOTAL"
              << std::setw(15) << totalOriginal
              << std::setw(15) << totalCompressed;

    if (!isDecompression)
        std::cout << std::setw(12) << std::fixed << std::setprecision(2)
                  << (totalOriginal > 0 ? (100.0 * totalCompressed / totalOriginal) : 0.0);

    std::cout << std::setw(12) << totalTime
              << successes << "/" << allStats.size()
              << "\n" << std::string(100, '=') << "\n\n";
}