#ifndef VIGENERE_H
#define VIGENERE_H

#include <string>
#include <vector>
#include "threadpool.h"

extern const size_t VIGENERE_BUFFER_SIZE;

// Estructura para estadísticas de encriptación
struct EncryptionStats {
    std::string filename;
    size_t fileSize;
    long long timeMs;
    bool success;
    std::string errorMsg;
    int threadId;
};

// Funciones principales de encriptación/desencriptación
void vigenere_encrypt(const std::string& inputFile, const std::string& outputFile, const std::string& key);
void vigenere_decrypt(const std::string& inputFile, const std::string& outputFile, const std::string& key);

// Funciones con estadísticas para procesamiento paralelo
EncryptionStats vigenere_encrypt_with_stats(
    const std::string& inputFile, 
    const std::string& outputFile, 
    const std::string& key,
    ThreadPool* pool
);

EncryptionStats vigenere_decrypt_with_stats(
    const std::string& inputFile, 
    const std::string& outputFile, 
    const std::string& key,
    ThreadPool* pool
);

// Función para imprimir estadísticas
void print_encryption_stats(const std::vector<EncryptionStats>& allStats, bool isDecryption = false);

// Validación de clave
bool validate_key(const std::string& key);

#endif // VIGENERE_H