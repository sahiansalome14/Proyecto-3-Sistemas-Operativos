#include "vigenere.h"
#include "posix_utils.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <vector>

const size_t VIGENERE_BUFFER_SIZE = 32 * 1024 * 1024; // 8 MB
const size_t VIGENERE_PARALLEL_THRESHOLD = 50 * 1024 * 1024; // 50 MB
const size_t VIGENERE_CHUNK_SIZE = 16 * 1024 * 1024; // 16 MB por chunk

std::mutex vigenere_console_mutex;

// Estructura para chunks de procesamiento paralelo
struct VigenereChunk {
    size_t id;
    size_t offset;
    size_t size;
    std::vector<unsigned char> data;
    bool success;
};

// Validar que la clave no esté vacía
bool validate_key(const std::string& key) {
    return !key.empty();
}

// Función helper para decidir si paralelizar
bool should_parallelize_vigenere(size_t fileSize) {
    return fileSize >= VIGENERE_PARALLEL_THRESHOLD;
}

//Archivos grandes
void vigenere_encrypt_parallel(const std::string& inputFile, 
                               const std::string& outputFile, 
                               const std::string& key) {
    // Abrir archivos
    int input_fd = open(inputFile.c_str(), O_RDONLY);
    if (input_fd < 0) {
        throw std::runtime_error("No se puede abrir el archivo de entrada: " + inputFile);
    }
    
    struct stat st;
    fstat(input_fd, &st);
    size_t fileSize = st.st_size;
    
    int output_fd = open(outputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0) {
        close(input_fd);
        throw std::runtime_error("No se puede crear el archivo de salida: " + outputFile);
    }
    
    // Calcular número de chunks
    size_t numChunks = (fileSize + VIGENERE_CHUNK_SIZE - 1) / VIGENERE_CHUNK_SIZE;
    std::vector<VigenereChunk> chunks(numChunks);
    
    // Inicializar chunks
    for (size_t i = 0; i < numChunks; i++) {
        chunks[i].id = i;
        chunks[i].offset = i * VIGENERE_CHUNK_SIZE;
        chunks[i].size = std::min(VIGENERE_CHUNK_SIZE, fileSize - chunks[i].offset);
        chunks[i].data.resize(chunks[i].size);
        chunks[i].success = false;
    }
    
    // Procesar chunks en paralelo
    const int numWorkers = std::min(static_cast<int>(std::thread::hardware_concurrency()), 
                                   static_cast<int>(numChunks));
    std::vector<std::thread> workers;
    std::atomic<size_t> nextChunk(0);
    std::atomic<bool> error_occurred(false);
    
    for (int t = 0; t < numWorkers; t++) {
        workers.emplace_back([&]() {
            while (!error_occurred.load()) {
                size_t chunkIdx = nextChunk.fetch_add(1);
                if (chunkIdx >= numChunks) break;
                
                VigenereChunk& chunk = chunks[chunkIdx];
                
                try {
                    // Leer chunk
                    ssize_t bytesRead = pread(input_fd, chunk.data.data(), 
                                             chunk.size, chunk.offset);
                    
                    if (bytesRead != (ssize_t)chunk.size) {
                        error_occurred = true;
                        break;
                    }
                    
                    // Calcular posición de clave para este chunk
                    size_t key_pos = chunk.offset % key.length();
                    size_t key_len = key.length();
                    
                    // Encriptar con loop unrolling (8 bytes a la vez)
                    size_t i = 0;
                    size_t limit = chunk.size - (chunk.size % 8);
                    
                    for (; i < limit; i += 8) {
                        chunk.data[i] = (chunk.data[i] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                        chunk.data[i+1] = (chunk.data[i+1] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                        chunk.data[i+2] = (chunk.data[i+2] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                        chunk.data[i+3] = (chunk.data[i+3] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                        chunk.data[i+4] = (chunk.data[i+4] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                        chunk.data[i+5] = (chunk.data[i+5] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                        chunk.data[i+6] = (chunk.data[i+6] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                        chunk.data[i+7] = (chunk.data[i+7] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                    }
                    
                    // Procesar bytes restantes
                    for (; i < chunk.size; i++) {
                        chunk.data[i] = (chunk.data[i] + 
                            static_cast<unsigned char>(key[key_pos % key_len])) % 256;
                        key_pos++;
                    }
                    
                    chunk.success = true;
                    
                } catch (...) {
                    error_occurred = true;
                    break;
                }
            }
        });
    }
    
    // Esperar a que terminen todos
    for (auto& w : workers) {
        w.join();
    }
    
    // Verificar errores
    if (error_occurred.load()) {
        close(input_fd);
        close(output_fd);
        throw std::runtime_error("Error durante la encriptación paralela");
    }
    
    // Escribir chunks EN ORDEN
    for (const auto& chunk : chunks) {
        if (!chunk.success) {
            close(input_fd);
            close(output_fd);
            throw std::runtime_error("Error procesando chunk");
        }
        
        ssize_t written = write(output_fd, chunk.data.data(), chunk.data.size());
        if (written != (ssize_t)chunk.data.size()) {
            close(input_fd);
            close(output_fd);
            throw std::runtime_error("Error escribiendo chunk");
        }
    }
    
    close(input_fd);
    close(output_fd);
}
//Desencriptación archivos grandes
void vigenere_decrypt_parallel(const std::string& inputFile, 
                               const std::string& outputFile, 
                               const std::string& key) {
    
    int input_fd = open(inputFile.c_str(), O_RDONLY);
    if (input_fd < 0) {
        throw std::runtime_error("No se puede abrir el archivo encriptado: " + inputFile);
    }
    
    struct stat st;
    fstat(input_fd, &st);
    size_t fileSize = st.st_size;
    
    int output_fd = open(outputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0) {
        close(input_fd);
        throw std::runtime_error("No se puede crear el archivo de salida: " + outputFile);
    }
    
    size_t numChunks = (fileSize + VIGENERE_CHUNK_SIZE - 1) / VIGENERE_CHUNK_SIZE;
    std::vector<VigenereChunk> chunks(numChunks);
    
    for (size_t i = 0; i < numChunks; i++) {
        chunks[i].id = i;
        chunks[i].offset = i * VIGENERE_CHUNK_SIZE;
        chunks[i].size = std::min(VIGENERE_CHUNK_SIZE, fileSize - chunks[i].offset);
        chunks[i].data.resize(chunks[i].size);
        chunks[i].success = false;
    }
    
    const int numWorkers = std::min(static_cast<int>(std::thread::hardware_concurrency()), 
                                   static_cast<int>(numChunks));
    std::vector<std::thread> workers;
    std::atomic<size_t> nextChunk(0);
    std::atomic<bool> error_occurred(false);
    
    for (int t = 0; t < numWorkers; t++) {
        workers.emplace_back([&]() {
            while (!error_occurred.load()) {
                size_t chunkIdx = nextChunk.fetch_add(1);
                if (chunkIdx >= numChunks) break;
                
                VigenereChunk& chunk = chunks[chunkIdx];
                
                try {
                    ssize_t bytesRead = pread(input_fd, chunk.data.data(), 
                                             chunk.size, chunk.offset);
                    
                    if (bytesRead != (ssize_t)chunk.size) {
                        error_occurred = true;
                        break;
                    }
                    
                    size_t key_pos = chunk.offset % key.length();
                    size_t key_len = key.length();
                    
                    // Desencriptar
                    for (size_t i = 0; i < chunk.size; i++) {
                        chunk.data[i] = (chunk.data[i] - 
                            static_cast<unsigned char>(key[key_pos % key_len]) + 256) % 256;
                        key_pos++;
                    }
                    
                    chunk.success = true;
                    
                } catch (...) {
                    error_occurred = true;
                    break;
                }
            }
        });
    }
    
    for (auto& w : workers) {
        w.join();
    }
    
    if (error_occurred.load()) {
        close(input_fd);
        close(output_fd);
        throw std::runtime_error("Error durante la desencriptación paralela");
    }
    
    for (const auto& chunk : chunks) {
        if (!chunk.success) {
            close(input_fd);
            close(output_fd);
            throw std::runtime_error("Error procesando chunk");
        }
        
        ssize_t written = write(output_fd, chunk.data.data(), chunk.data.size());
        if (written != (ssize_t)chunk.data.size()) {
            close(input_fd);
            close(output_fd);
            throw std::runtime_error("Error escribiendo chunk");
        }
    }
    
    close(input_fd);
    close(output_fd);
}
void vigenere_encrypt(const std::string& inputFile, const std::string& outputFile, const std::string& key) {
    if (!validate_key(key)) {
        throw std::runtime_error("La clave de encriptación no puede estar vacía");
    }
    
    size_t fileSize = posix_file_size(inputFile);
    
    // Si el archivo es grande, usar versión paralela
    if (should_parallelize_vigenere(fileSize)) {
        vigenere_encrypt_parallel(inputFile, outputFile, key);
        return;
    }
    
    // Versión secuencial para archivos pequeños
    int input_fd = open(inputFile.c_str(), O_RDONLY);
    if (input_fd < 0) {
        throw std::runtime_error("No se puede abrir el archivo de entrada: " + inputFile);
    }
    
    int output_fd = open(outputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0) {
        close(input_fd);
        throw std::runtime_error("No se puede crear el archivo de salida: " + outputFile);
    }
    
    unsigned char* buffer = new unsigned char[VIGENERE_BUFFER_SIZE];
    ssize_t bytes_read;
    size_t key_len = key.length();
    size_t key_pos = 0;
    
    while ((bytes_read = read(input_fd, buffer, VIGENERE_BUFFER_SIZE)) > 0) {
        for (ssize_t i = 0; i < bytes_read; i++) {
            buffer[i] = (buffer[i] + static_cast<unsigned char>(key[key_pos % key_len])) % 256;
            key_pos++;
        }
        
        ssize_t bytes_written = write(output_fd, buffer, bytes_read);
        if (bytes_written != bytes_read) {
            delete[] buffer;
            close(input_fd);
            close(output_fd);
            throw std::runtime_error("Error al escribir en el archivo de salida");
        }
    }
    
    delete[] buffer;
    close(input_fd);
    close(output_fd);
    
    if (bytes_read < 0) {
        throw std::runtime_error("Error al leer el archivo de entrada");
    }
}

void vigenere_decrypt(const std::string& inputFile, const std::string& outputFile, const std::string& key) {
    if (!validate_key(key)) {
        throw std::runtime_error("La clave de desencriptación no puede estar vacía");
    }
    
    size_t fileSize = posix_file_size(inputFile);
    
    // Si el archivo es grande, usar versión paralela
    if (should_parallelize_vigenere(fileSize)) {
        vigenere_decrypt_parallel(inputFile, outputFile, key);
        return;
    }
    
    // Versión secuencial para archivos pequeños
    int input_fd = open(inputFile.c_str(), O_RDONLY);
    if (input_fd < 0) {
        throw std::runtime_error("No se puede abrir el archivo encriptado: " + inputFile);
    }
    
    int output_fd = open(outputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0) {
        close(input_fd);
        throw std::runtime_error("No se puede crear el archivo de salida: " + outputFile);
    }
    
    unsigned char* buffer = new unsigned char[VIGENERE_BUFFER_SIZE];
    ssize_t bytes_read;
    size_t key_len = key.length();
    size_t key_pos = 0;
    
    while ((bytes_read = read(input_fd, buffer, VIGENERE_BUFFER_SIZE)) > 0) {
        for (ssize_t i = 0; i < bytes_read; i++) {
            buffer[i] = (buffer[i] - static_cast<unsigned char>(key[key_pos % key_len]) + 256) % 256;
            key_pos++;
        }
        
        ssize_t bytes_written = write(output_fd, buffer, bytes_read);
        if (bytes_written != bytes_read) {
            delete[] buffer;
            close(input_fd);
            close(output_fd);
            throw std::runtime_error("Error al escribir en el archivo de salida");
        }
    }
    
    delete[] buffer;
    close(input_fd);
    close(output_fd);
    
    if (bytes_read < 0) {
        throw std::runtime_error("Error al leer el archivo de entrada");
    }
}
//Estadisticas 
EncryptionStats vigenere_encrypt_with_stats(
    const std::string& inputFile, 
    const std::string& outputFile, 
    const std::string& key,
    ThreadPool* pool
) {
    EncryptionStats stats;
    stats.filename = inputFile;
    stats.success = false;
    stats.threadId = pool->getThreadId();
    
    {
        std::lock_guard<std::mutex> lock(vigenere_console_mutex);
        std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] 🔐 Encriptando: " 
                  << inputFile << std::endl;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    try {
        stats.fileSize = posix_file_size(inputFile);
        
        std::string ext = posix_get_extension(inputFile);
        if (ext == ".enc") {
            throw std::runtime_error("El archivo ya está encriptado (.enc)");
        }
    
        vigenere_encrypt(inputFile, outputFile, key);
        stats.success = true;
        
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        {
            std::lock_guard<std::mutex> lock(vigenere_console_mutex);
            std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ✓ Completado: " 
                      << inputFile 
                      << " (" << stats.fileSize << " bytes, " 
                      << stats.timeMs << " ms)" << std::endl;
        }
        
    } catch (const std::exception& e) {
        stats.errorMsg = e.what();
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        std::lock_guard<std::mutex> lock(vigenere_console_mutex);
        std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ✗ Error: " 
                  << inputFile << " - " << e.what() << std::endl;
    }
    
    return stats;
}
EncryptionStats vigenere_decrypt_with_stats(
    const std::string& inputFile, 
    const std::string& outputFile, 
    const std::string& key,
    ThreadPool* pool
) {
    EncryptionStats stats;
    stats.filename = inputFile;
    stats.success = false;
    stats.threadId = pool->getThreadId();
    
    {
        std::lock_guard<std::mutex> lock(vigenere_console_mutex);
        std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] 🔓 Desencriptando: " 
                  << inputFile << std::endl;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    try {
        stats.fileSize = posix_file_size(inputFile);
        
        vigenere_decrypt(inputFile, outputFile, key);
        stats.success = true;
        
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        {
            std::lock_guard<std::mutex> lock(vigenere_console_mutex);
            std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ✓ Completado: " 
                      << inputFile 
                      << " (" << stats.fileSize << " bytes, " 
                      << stats.timeMs << " ms)" << std::endl;
        }
        
    } catch (const std::exception& e) {
        stats.errorMsg = e.what();
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        std::lock_guard<std::mutex> lock(vigenere_console_mutex);
        std::cout << "[Hilo " << std::setw(2) << stats.threadId << "] ✗ Error: " 
                  << inputFile << " - " << e.what() << std::endl;
    }
    
    return stats;
}
void print_encryption_stats(const std::vector<EncryptionStats>& allStats, bool isDecryption) {
    size_t totalSize = 0, successes = 0;
    long long totalTime = 0;
    
    std::cout << "\n" << std::string(100, '=') << "\n";
    if (isDecryption)
        std::cout << "RESUMEN DE DESENCRIPTACIÓN\n";
    else
        std::cout << "RESUMEN DE ENCRIPTACIÓN\n";
    std::cout << std::string(100, '=') << "\n\n";
    std::cout << std::left << std::setw(50) << "Archivo" 
              << std::setw(20) << "Tamaño (bytes)"
              << std::setw(15) << "Tiempo (ms)" 
              << "Estado\n" << std::string(100, '-') << "\n";
    
    for (const auto& stat : allStats) {
        std::cout << std::left << std::setw(50) << stat.filename.substr(0, 49);
        if (stat.success) {
            std::cout << std::setw(20) << stat.fileSize 
                      << std::setw(15) << stat.timeMs 
                      << "✓ OK\n";
            totalSize += stat.fileSize;
            totalTime += stat.timeMs;
            successes++;
        } else {
            std::cout << std::setw(20) << "-" 
                      << std::setw(15) << stat.timeMs 
                      << "✗ ERROR: " << stat.errorMsg << "\n";
        }
    }
    
    std::cout << std::string(100, '-') << "\n" 
              << std::left << std::setw(50) << "TOTAL"
              << std::setw(20) << totalSize 
              << std::setw(15) << totalTime 
              << successes << "/" << allStats.size() << "\n"
              << std::string(100, '=') << "\n\n";
}