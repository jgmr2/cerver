/*
 * scaffold.h - crea un proyecto cerver nuevo a partir del esqueleto
 * embebido en el binario (ver boilerplate_zip.h / tools/pack_boilerplate.py)
 *
 * DESCRIPCION
 *     A diferencia de codegen.c (que agrega endpoints CRUD a un proyecto
 *     cerver que ya existe), esto crea el proyecto en si: dbfiller es un
 *     binario standalone, no necesita tener un checkout de cerver al lado
 *     para arrancar un cliente nuevo.
 */
#ifndef DBFILLER_SCAFFOLD_H
#define DBFILLER_SCAFFOLD_H

#include <stddef.h>

/*
 * scaffold_new_project - extrae el esqueleto embebido a dest_dir
 *
 * dest_dir se crea si no existe. Si ya existe y NO esta vacio, falla (err
 * lo indica) en vez de arriesgarse a mezclar archivos de un proyecto
 * existente con el esqueleto - el llamador debe elegir una carpeta nueva
 * o vacia.
 *
 * Requiere el binario `unzip` en PATH (se shell-ea, no hay unzip propio
 * vendorizado).
 *
 * Retorna 0 en exito, -1 en error (err queda lleno).
 */
int scaffold_new_project(const char *dest_dir, char *err, size_t err_len);

#endif
