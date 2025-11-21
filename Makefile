# ==========================================
# Proyecto SO3 - Compresor + Encriptador
# ==========================================

CXX = g++
CXXFLAGS = -std=c++17 -O3 -pthread -Wall -Wextra -march=native

# Ejecutable final
TARGET = proyecto

# Archivos fuente
SOURCES = main.cpp huffman.cpp vigenere.cpp posix_utils.cpp threadpool.cpp
OBJECTS = $(SOURCES:.cpp=.o)
HEADERS = huffman.h vigenere.h posix_utils.h threadpool.h

# Regla principal
all: $(TARGET)

# Enlazar ejecutable final
$(TARGET): $(OBJECTS)
	@echo "🔧 Enlazando ejecutable..."
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "✅ Compilación completa: $(TARGET)"

# Compilar archivos .cpp -> .o
%.o: %.cpp $(HEADERS)
	@echo "Compilando $< ..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Limpiar archivos generados
clean:
	@echo "🧹 Limpiando..."
	rm -f $(OBJECTS) $(TARGET)
	@echo "✅ Proyecto limpio"

.PHONY: all clean
