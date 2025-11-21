# Proyecto-3-Sistemas-Operativos

## Autores
- Sahian Salomé Gutiérrez Ossa
- Kadiha Nahir Muhammad Orta
- Mariamny Del Valle Ramírez Telles

## Descripción
Este proyecto implementa un sistema completo para compresión, descompresión, encriptación, desencriptación y procesamiento en batch, utilizando algoritmos avanzados y paralelismo de alto rendimiento.

Incluye:

 - Compresión Huffman optimizada (con pipeline, chunks y paralelismo).

 - Encriptación Vigenère con versión secuencial y paralela.

 - ThreadPool personalizado para ejecución concurrente.

 - Modo batch para procesar directorios enteros en paralelo.

 - I/O optimizado con POSIX (pread, write, open, etc.)

## Requisitos
- Tener instalado `make`
- Compilador compatible con C/C++
  
## Compilación
Para compilar el proyecto, abre una terminal en la carpeta raíz del proyecto y ejecuta:

```bash
make
```
Esto generará el ejecutable correspondiente según la configuración del Makefile.

## Ejecución

Para ejecutar el programa compilado:

```bash
./proyecto
```

## Limpieza

Para eliminar archivos generados durante la compilación, puedes usar:

```bash
make clean
```
