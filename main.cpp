#include "huffman.h"
#include "vigenere.h"
#include "posix_utils.h"
#include "threadpool.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <unistd.h>

void print_usage(const char* program_name) {
    std::cout << "Uso:\n"
              << "  COMPRESIÓN/DESCOMPRESIÓN:\n"
              << "    Archivo único:\n"
              << "      " << program_name << " -c <entrada> <salida>\n"
              << "      " << program_name << " -d <entrada> <salida>\n"
              << "    Batch (directorio):\n"
              << "      " << program_name << " -cb <dir_entrada> <dir_salida> [extensión]\n"
              << "      " << program_name << " -db <dir_entrada> <dir_salida>\n\n"
              << "  ENCRIPTACIÓN/DESENCRIPTACIÓN:\n"
              << "    Archivo único:\n"
              << "      " << program_name << " -e <entrada> <salida> -k <clave>\n"
              << "      " << program_name << " -u <entrada> <salida> -k <clave>\n"
              << "    Batch (directorio):\n"
              << "      " << program_name << " -eb <dir_entrada> <dir_salida> -k <clave> [extensión]\n"
              << "      " << program_name << " -ub <dir_entrada> <dir_salida> -k <clave>\n\n"
              << "  COMBINADO (comprimir y encriptar):\n"
              << "    Archivo único:\n"
              << "      " << program_name << " -ce <entrada> <salida> -k <clave>\n"
              << "    Batch (directorio):\n"
              << "      " << program_name << " -ceb <dir_entrada> <dir_salida> -k <clave> [extensión]\n"
              << "      " << program_name << " -dub <dir_entrada> <dir_salida> -k <clave>\n\n"
              << "Opciones:\n"
              << "  -c       Comprimir\n"
              << "  -d       Descomprimir\n"
              << "  -e       Encriptar\n"
              << "  -u       Desencriptar (unlock)\n"
              << "  -ce      Comprimir y encriptar\n"
              << "  -du      Desencriptar y descomprimir\n"
              << "  -ceb     Comprimir y encriptar (batch)\n"
              << "  -dub     Desencriptar y descomprimir (batch)\n"
              << "  -b       Modo batch (procesar directorio)\n"
              << "  -k       Clave de encriptación/desencriptación\n";
}

std::string extract_key_from_args(int argc, char* argv[], int& key_index) {
    for (int i = 1; i < argc - 1; i++) {
        if (std::string(argv[i]) == "-k") {
            key_index = i;
            return argv[i + 1];
        }
    }
    return "";
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        
        std::string mode = argv[1];
        
        // modo archivo único - compresión/descompresión
        if (mode == "-c" || mode == "-d") {
            if (argc != 4) {
                std::cerr << "Error: Se requieren 3 argumentos\n";
                return 1;
            }
            std::string inputFile = argv[2];
            std::string outputFile = argv[3];
            
            auto start = std::chrono::high_resolution_clock::now();
            if (mode == "-c") {
                compress(inputFile, outputFile);
                std::cout << "✓ Comprimido: " << inputFile << " -> " << outputFile << "\n";
            } else {
                decompress(inputFile, outputFile);
                std::cout << "✓ Descomprimido: " << inputFile << " -> " << outputFile << "\n";
            }
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            std::cout << "Tiempo: " << duration.count() << " ms\n";
            return 0;
        }
        
        // modo archivo único - encriptación/desencriptación
        if (mode == "-e" || mode == "-u") {
            int key_index = -1;
            std::string key = extract_key_from_args(argc, argv, key_index);
            
            if (key.empty()) {
                std::cerr << "Error: Se requiere una clave con -k\n";
                print_usage(argv[0]);
                return 1;
            }
            
            if (argc < 5) {
                std::cerr << "Error: Se requieren entrada y salida\n";
                return 1;
            }
            
            std::string inputFile = argv[2];
            std::string outputFile = argv[3];
            
            auto start = std::chrono::high_resolution_clock::now();
            if (mode == "-e") {
                vigenere_encrypt(inputFile, outputFile, key);
                std::cout << "🔒 Encriptado: " << inputFile << " -> " << outputFile << "\n";
            } else {
                vigenere_decrypt(inputFile, outputFile, key);
                std::cout << "🔓 Desencriptado: " << inputFile << " -> " << outputFile << "\n";
            }
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            std::cout << "Tiempo: " << duration.count() << " ms\n";
            return 0;
        }
        
        // modo combinado - comprimir y encriptar (archivo único)
        if (mode == "-ce") {
            int key_index = -1;
            std::string key = extract_key_from_args(argc, argv, key_index);
            
            if (key.empty()) {
                std::cerr << "Error: Se requiere una clave con -k\n";
                print_usage(argv[0]);
                return 1;
            }
            
            if (argc < 5) {
                std::cerr << "Error: Se requieren entrada y salida\n";
                return 1;
            }
            
            std::string inputFile = argv[2];
            std::string outputFile = argv[3];
            std::string tempFile = outputFile + ".tmp";
            
            auto start = std::chrono::high_resolution_clock::now();
            
            compress(inputFile, tempFile);
            std::cout << "✓ Comprimido: " << inputFile << " -> " << tempFile << "\n";
            
            vigenere_encrypt(tempFile, outputFile, key);
            std::cout << "🔒 Encriptado: " << tempFile << " -> " << outputFile << "\n";
            
            unlink(tempFile.c_str());
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            std::cout << "Tiempo total: " << duration.count() << " ms\n";
            return 0;
        }
        
        // modo combinado - desencriptar y descomprimir (archivo único)
        if (mode == "-du") {
            int key_index = -1;
            std::string key = extract_key_from_args(argc, argv, key_index);

            if (key.empty()) {
                std::cerr << "Error: Se requiere una clave con -k\n";
                print_usage(argv[0]);
                return 1;
            }

            if (argc < 5) {
                std::cerr << "Error: Se requieren entrada y salida\n";
                return 1;
            }

            std::string inputFile = argv[2];
            std::string outputFile = argv[3];
            std::string tempFile = outputFile + ".tmp";

            auto start = std::chrono::high_resolution_clock::now();

            //  Desencriptar primero
            vigenere_decrypt(inputFile, tempFile, key);
            std::cout << "🔓 Desencriptado: " << inputFile << " -> " << tempFile << "\n";

            //  Luego descomprimir el archivo temporal
            decompress(tempFile, outputFile);
            std::cout << "📂 Descomprimido: " << tempFile << " -> " << outputFile << "\n";

            //  Eliminar el temporal
            unlink(tempFile.c_str());

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            std::cout << "Tiempo total: " << duration.count() << " ms\n";

            return 0;
        }

        
        // modo batch combinado - comprimir y encriptar
        if (mode == "-ceb") {
            int key_index = -1;
            std::string key = extract_key_from_args(argc, argv, key_index);
            
            if (key.empty()) {
                std::cerr << "Error: Se requiere una clave con -k\n";
                print_usage(argv[0]);
                return 1;
            }
            
            if (argc < 6) {
                std::cerr << "Error: Se requieren directorios de entrada y salida\n";
                return 1;
            }
            
            std::string inputDir = argv[2];
            std::string outputDir = argv[3];
            std::string extension = "";
            
            if (key_index + 2 < argc) {
                extension = argv[key_index + 2];
            }
            
            if (!posix_is_directory(inputDir)) {
                std::cerr << "Error: El directorio de entrada no existe: " << inputDir << "\n";
                return 1;
            }
            
            posix_create_directories(outputDir);
            
            std::vector<std::string> allFiles;
            posix_recursive_directory_iterator(inputDir, allFiles);
            
            std::vector<std::tuple<std::string, std::string, std::string>> fileTuples;
            
            for (const auto& inputPath : allFiles) {
                std::string ext = posix_get_extension(inputPath);
                if (!extension.empty() && ext != extension) continue;
                
                std::string relativePath = posix_relative_path(inputPath, inputDir);
                std::string outputPath = posix_join_path(outputDir, relativePath) + ".huf.enc";
                std::string tempPath = outputPath + ".tmp";
                
                std::string parentDir = posix_get_parent_path(outputPath);
                posix_create_directories(parentDir);
                
                fileTuples.emplace_back(inputPath, tempPath, outputPath);
            }
            
            if (fileTuples.empty()) {
                std::cout << "No se encontraron archivos para procesar\n";
                return 0;
            }
            
            std::cout << "\n" << std::string(60, '=') << "\n";
            std::cout << "PROCESAMIENTO PARALELO - COMPRESIÓN + ENCRIPTACIÓN\n";
            std::cout << std::string(60, '=') << "\n";
            std::cout << "Archivos a procesar: " << fileTuples.size() << "\n";
            std::cout << "Hilos disponibles:   " << NUM_THREADS << "\n";
            std::cout << "Hilos activos:       " << std::min(static_cast<size_t>(NUM_THREADS), fileTuples.size()) << "\n";
            std::cout << std::string(60, '=') << "\n\n";
            
            ThreadPool pool(NUM_THREADS);
            std::vector<std::future<void>> futures;
            futures.reserve(fileTuples.size());
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            for (const auto& [input, temp, output] : fileTuples) {
                futures.push_back(pool.enqueue([input, temp, output, key, &pool]() {
                    compress(input, temp);
                    vigenere_encrypt(temp, output, key);
                    unlink(temp.c_str());
                    std::cout << "✓ [Thread " << pool.getThreadId() << "] " 
                              << posix_get_filename(input) << " -> " 
                              << posix_get_filename(output) << "\n";
                }));
            }
            
            std::cout << "Iniciando procesamiento paralelo...\n" << std::string(60, '-') << "\n";
            
            for (auto& future : futures) {
                future.get();
            }
            
            std::cout << std::string(60, '-') << "\n";
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            
            std::cout << "\nTiempo total: " << totalTime.count() << " ms\n";
            std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
                      << (fileTuples.size() * 1000.0 / totalTime.count()) << " archivos/s\n\n";
            
            return 0;
        }
        
        // modo batch - desencriptar y descomprimir
        if (mode == "-dub") {
            int key_index = -1;
            std::string key = extract_key_from_args(argc, argv, key_index);
            
            if (key.empty()) {
                std::cerr << "Error: Se requiere una clave con -k\n";
                print_usage(argv[0]);
                return 1;
            }
            
            if (argc < 6) {
                std::cerr << "Error: Se requieren directorios de entrada y salida\n";
                return 1;
            }
            
            std::string inputDir = argv[2];
            std::string outputDir = argv[3];
            
            if (!posix_is_directory(inputDir)) {
                std::cerr << "Error: El directorio de entrada no existe: " << inputDir << "\n";
                return 1;
            }
            
            posix_create_directories(outputDir);
            
            std::vector<std::string> allFiles;
            posix_recursive_directory_iterator(inputDir, allFiles);
            
            std::vector<std::tuple<std::string, std::string, std::string>> fileTuples;
            
            for (const auto& inputPath : allFiles) {
                std::string ext = posix_get_extension(inputPath);
                if (ext != ".enc") continue;
                
                std::string relativePath = posix_relative_path(inputPath, inputDir);
                std::string outputPath = posix_join_path(outputDir, relativePath);
                
                std::string parent = posix_get_parent_path(outputPath);
                std::string filename = posix_get_filename(outputPath);
                
                if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".enc") {
                    filename = filename.substr(0, filename.size() - 4);
                }
                if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".huf") {
                    filename = filename.substr(0, filename.size() - 4);
                }
                
                outputPath = posix_join_path(parent, filename);
                std::string tempPath = outputPath + ".tmp";
                
                std::string parentDir = posix_get_parent_path(outputPath);
                posix_create_directories(parentDir);
                
                fileTuples.emplace_back(inputPath, tempPath, outputPath);
            }
            
            if (fileTuples.empty()) {
                std::cout << "No se encontraron archivos .huf.enc para procesar\n";
                return 0;
            }
            
            std::cout << "\n" << std::string(60, '=') << "\n";
            std::cout << "PROCESAMIENTO PARALELO - DESENCRIPTACIÓN + DESCOMPRESIÓN\n";
            std::cout << std::string(60, '=') << "\n";
            std::cout << "Archivos a procesar: " << fileTuples.size() << "\n";
            std::cout << "Hilos disponibles:   " << NUM_THREADS << "\n";
            std::cout << "Hilos activos:       " << std::min(static_cast<size_t>(NUM_THREADS), fileTuples.size()) << "\n";
            std::cout << std::string(60, '=') << "\n\n";
            
            ThreadPool pool(NUM_THREADS);
            std::vector<std::future<void>> futures;
            futures.reserve(fileTuples.size());
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            for (const auto& [input, temp, output] : fileTuples) {
                futures.push_back(pool.enqueue([input, temp, output, key, &pool]() {
                    vigenere_decrypt(input, temp, key);
                    decompress(temp, output);
                    unlink(temp.c_str());
                    std::cout << "✓ [Thread " << pool.getThreadId() << "] " 
                              << posix_get_filename(input) << " -> " 
                              << posix_get_filename(output) << "\n";
                }));
            }
            
            std::cout << "Iniciando procesamiento paralelo...\n" << std::string(60, '-') << "\n";
            
            for (auto& future : futures) {
                future.get();
            }
            
            std::cout << std::string(60, '-') << "\n";
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            
            std::cout << "\nTiempo total: " << totalTime.count() << " ms\n";
            std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
                      << (fileTuples.size() * 1000.0 / totalTime.count()) << " archivos/s\n\n";
            
            return 0;
        }
    
        
        // modo batch - compresión/descompresión
        if (mode == "-cb" || mode == "-db") {
            if (argc < 4) {
                std::cerr << "Error: Se requieren al menos 3 argumentos\n";
                return 1;
            }
            
            std::string inputDir = argv[2];
            std::string outputDir = argv[3];
            std::string extension = (argc >= 5) ? argv[4] : "";
            
            if (!posix_is_directory(inputDir)) {
                std::cerr << "Error: El directorio de entrada no existe: " << inputDir << "\n";
                return 1;
            }
            
            posix_create_directories(outputDir);
            
            std::vector<std::string> allFiles;
            posix_recursive_directory_iterator(inputDir, allFiles);
            
            std::vector<std::pair<std::string, std::string>> filePairs;
            
            for (const auto& inputPath : allFiles) {
                std::string ext = posix_get_extension(inputPath);
                
                if (!extension.empty() && ext != extension) continue;
                if (mode == "-db" && ext != ".huf") continue;
                
                std::string relativePath = posix_relative_path(inputPath, inputDir);
                std::string outputPath = posix_join_path(outputDir, relativePath);
                
                if (mode == "-cb") {
                    outputPath = outputPath + ".huf";
                } else {
                    std::string parent = posix_get_parent_path(outputPath);
                    std::string stem = posix_get_stem(outputPath);
                    outputPath = posix_join_path(parent, stem);
                }
                
                std::string parentDir = posix_get_parent_path(outputPath);
                posix_create_directories(parentDir);
                
                filePairs.emplace_back(inputPath, outputPath);
            }
            
            if (filePairs.empty()) {
                std::cout << "No se encontraron archivos para procesar\n";
                return 0;
            }
            
            std::cout << "\n" << std::string(60, '=') << "\n";
            if (mode == "-cb")
                std::cout << "PROCESAMIENTO PARALELO - COMPRESIÓN\n";
            else
                std::cout << "PROCESAMIENTO PARALELO - DESCOMPRESIÓN\n";
            std::cout << std::string(60, '=') << "\n";
            std::cout << "Archivos a procesar: " << filePairs.size() << "\n";
            std::cout << "Hilos disponibles:   " << NUM_THREADS << "\n";
            std::cout << "Hilos activos:       " << std::min(static_cast<size_t>(NUM_THREADS), filePairs.size()) << "\n";
            std::cout << "Modo:                " << (mode == "-cb" ? "Compresión" : "Descompresión") << "\n";
            std::cout << std::string(60, '=') << "\n\n";
            
            ThreadPool pool(NUM_THREADS);
            std::vector<std::future<CompressionStats>> futures;
            futures.reserve(filePairs.size());
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            for (size_t i = 0; i < filePairs.size(); ++i) {
                const std::string& input = filePairs[i].first;
                const std::string& output = filePairs[i].second;
                
                if (mode == "-cb") {
                    futures.push_back(pool.enqueue(compressWithStats, input, output, &pool));
                } else {
                    futures.push_back(pool.enqueue(decompressWithStats, input, output, &pool));
                }
            }
            
            std::vector<CompressionStats> allStats;
            allStats.reserve(futures.size());
            
            std::cout << "\nIniciando procesamiento paralelo...\n" << std::string(60, '-') << "\n";
            
            for (size_t i = 0; i < futures.size(); ++i) {
                allStats.push_back(futures[i].get());
            }
            
            std::cout << std::string(60, '-') << "\n";
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            
            printStats(allStats, mode == "-db");

            std::cout << "Tiempo total: " << totalTime.count() << " ms\n";
            std::cout << "Throughput: " << std::fixed << std::setprecision(2)
                      << (filePairs.size() * 1000.0 / totalTime.count()) << " archivos/s\n\n";
            
            return 0;
        }
        
        // modo batch - encriptación/desencriptación
        if (mode == "-eb" || mode == "-ub") {
            int key_index = -1;
            std::string key = extract_key_from_args(argc, argv, key_index);
            
            if (key.empty()) {
                std::cerr << "Error: Se requiere una clave con -k\n";
                print_usage(argv[0]);
                return 1;
            }
            
            if (argc < 6) {
                std::cerr << "Error: Se requieren directorios de entrada y salida\n";
                return 1;
            }
            
            std::string inputDir = argv[2];
            std::string outputDir = argv[3];
            std::string extension = "";
            
            if (key_index + 2 < argc) {
                extension = argv[key_index + 2];
            }
            
            if (!posix_is_directory(inputDir)) {
                std::cerr << "Error: El directorio de entrada no existe: " << inputDir << "\n";
                return 1;
            }
            
            posix_create_directories(outputDir);
            
            std::vector<std::string> allFiles;
            posix_recursive_directory_iterator(inputDir, allFiles);
            
            std::vector<std::pair<std::string, std::string>> filePairs;
            
            for (const auto& inputPath : allFiles) {
                std::string ext = posix_get_extension(inputPath);
                
                if (!extension.empty() && ext != extension) continue;
                if (mode == "-ub" && ext != ".enc") continue;
                
                std::string relativePath = posix_relative_path(inputPath, inputDir);
                std::string outputPath = posix_join_path(outputDir, relativePath);
                
                if (mode == "-eb") {
                    outputPath = outputPath + ".enc";
                } else {
                    std::string parent = posix_get_parent_path(outputPath);
                    std::string stem = posix_get_stem(outputPath);
                    outputPath = posix_join_path(parent, stem);
                }
                
                std::string parentDir = posix_get_parent_path(outputPath);
                posix_create_directories(parentDir);
                
                filePairs.emplace_back(inputPath, outputPath);
            }
            
            if (filePairs.empty()) {
                std::cout << "No se encontraron archivos para procesar\n";
                return 0;
            }
            
            std::cout << "\n" << std::string(60, '=') << "\n";
            std::cout << "PROCESAMIENTO PARALELO - ENCRIPTACIÓN\n";
            std::cout << std::string(60, '=') << "\n";
            std::cout << "Archivos a procesar: " << filePairs.size() << "\n";
            std::cout << "Hilos disponibles:   " << NUM_THREADS << "\n";
            std::cout << "Hilos activos:       " << std::min(static_cast<size_t>(NUM_THREADS), filePairs.size()) << "\n";
            std::cout << "Modo:                " << (mode == "-eb" ? "Encriptación" : "Desencriptación") << "\n";
            std::cout << "Algoritmo:           Vigenère\n";
            std::cout << std::string(60, '=') << "\n\n";
            
            ThreadPool pool(NUM_THREADS);
            std::vector<std::future<EncryptionStats>> futures;
            futures.reserve(filePairs.size());
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            for (size_t i = 0; i < filePairs.size(); ++i) {
                const std::string& input = filePairs[i].first;
                const std::string& output = filePairs[i].second;
                
                if (mode == "-eb") {
                    futures.push_back(pool.enqueue(vigenere_encrypt_with_stats, input, output, key, &pool));
                } else {
                    futures.push_back(pool.enqueue(vigenere_decrypt_with_stats, input, output, key, &pool));
                }
            }
            
            std::vector<EncryptionStats> allStats;
            allStats.reserve(futures.size());
            
            std::cout << "\nIniciando procesamiento paralelo...\n" << std::string(60, '-') << "\n";
            
            for (size_t i = 0; i < futures.size(); ++i) {
                allStats.push_back(futures[i].get());
            }
            
            std::cout << std::string(60, '-') << "\n";
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            
            print_encryption_stats(allStats, mode == "-ub");
            
            std::cout << "Tiempo total: " << totalTime.count() << " ms\n";
            std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
                      << (filePairs.size() * 1000.0 / totalTime.count()) << " archivos/s\n\n";
            
            return 0;
        }
        
        std::cerr << "Modo inválido: " << mode << "\n";
        print_usage(argv[0]);
        return 1;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}