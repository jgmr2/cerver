---
name: io-uring-c-server-engine
description: 'Disena e implementa el motor de un servidor HTTP en C con io_uring/liburing, pthreads, Memory Arenas y Object Pools sin malloc/free en fast path. Usar cuando se necesite generar core/server.c, core/server.h, utils/events.h y main.c con arquitectura Master-Readers-Writer, sincronizacion segura y enfoque anti-race/anti-leaks.'
argument-hint: 'Pega arquitectura de carpetas, reglas de memoria/concurrencia y entregables esperados.'
user-invocable: true
---

# io_uring C Server Engine

## Que Produce
Este skill genera una implementacion paso a paso para integrar un motor de servidor en C de alto rendimiento, adaptado al arbol de archivos del proyecto:
- Estructuras de contexto y eventos en `utils/events.h`
- API publica y contratos en `core/server.h`
- Implementacion de Master, Readers, Writer, arenas, pool y apagado limpio en `core/server.c`
- Ejemplo de arranque y ciclo de vida en `main.c`

Incluye comentarios en el codigo sobre:
- Como las Memory Arenas minimizan fragmentacion al usar allocacion lineal y reset por ciclo
- Como el Object Pool evita use-after-free al reciclar contextos con ownership claro

## Cuando Usarlo
Usar este skill cuando aparezcan requisitos como:
- `io_uring`, `liburing`, `pthreads`, `C server`, `event loop`, `lock-free`, `spinlock`
- Restriccion de no usar heap (`malloc/free`) en ruta caliente
- Necesidad de arquitectura Master + workers de lectura + writer dedicado
- Requisito explicito de robustez ante segfaults, leaks y race conditions

No usar este skill para:
- Cambios pequenos de endpoints sin tocar el motor de concurrencia
- Refactors UI/frontend
- Proyectos que no usan Linux + io_uring

## Entradas Requeridas
Antes de generar codigo, confirmar:
1. Arbol de directorios objetivo (o archivos exactos a tocar)
2. Version minima de kernel/liburing esperada
3. Limites: max conexiones, tamano de buffers, profundidad del ring
4. Politica de sincronizacion preferida (`pthread_mutex_t` o spinlock)
5. Comportamiento de cierre (`SIGINT`, drain de colas, timeout de salida)

## Defaults Recomendados
Si el usuario no especifica parametros, usar:
1. Compatibilidad: kernel >= 5.15 y liburing estable reciente
2. Sincronizacion del ring compartido: `pthread_mutex_t`
3. Escritura: 1 writer dedicado con cola MPSC desde readers
4. Heap policy: solo asignaciones en fase de inicializacion

## Procedimiento
1. Inspeccionar proyecto y ubicar archivos actuales
2. Definir contratos de tipos compartidos:
- `request_context` y metadatos en `utils/events.h`
- enum/event tags para distinguir operaciones READ/WRITE/ACCEPT/CLOSE
3. Disenar memoria preasignada:
- Arena por hilo (reader/writer) con offset lineal
- Pool de `request_context` con estado de ownership
4. Definir API del servidor en `core/server.h`:
- Config struct (ring depth, workers, buffer sizes)
- Funciones `server_init`, `server_start`, `server_stop`, `server_destroy`
5. Implementar `core/server.c` por fases:
- Master: init ring, socket, pools/arenas, spawn threads
- Readers: submit/read, parse HTTP via `utils/http/`, route via `routes/` y `controllers/`
- Writer: cola de respuestas y `IORING_OP_WRITE`/`IORING_OP_SEND`
- Recycle seguro de contextos al completar CQE
6. Integrar arranque en `main.c`:
- Cargar config, inicializar servidor, manejar senales, shutdown limpio
7. Verificar invariantes y corregir:
- Cero heap allocations en fast path
- Cero dobles liberaciones / use-after-free
- Sin data races en ring y colas inter-thread

## Logica de Decision
- Si se detecta heap en ruta caliente:
  reemplazar por reservas en arena/pool y documentar donde se mantiene heap permitido (solo init)
- Si el ring es compartido por multiples hilos:
  aplicar mecanismo de sincronizacion explicito antes de `io_uring_get_sqe`/`io_uring_submit`
- Si falta separacion clara de ownership de `request_context`:
  introducir maquina de estados (`FREE -> IN_READ -> IN_ROUTE -> IN_WRITE -> FREE`)
- Si parseo/ruteo actual no es thread-safe:
  aislar estado mutable por hilo o proteger secciones criticas

## Criterios de Calidad (Completion Checks)
Checklist minimo antes de considerar completo:
1. Compila sin warnings criticos en archivos modificados
2. `core/server.c` no usa `malloc/free` en fast path
3. `utils/events.h` define contexto reutilizable compatible con `user_data`
4. Reuso de objetos del pool ocurre en exito y error
5. Reset de arena por request completada o batch definido
6. Manejo de errores de syscalls con rollback consistente
7. Cierre limpio: stop flag, join de hilos, cierre de sockets/ring
8. Comentarios tecnicos claros sobre fragmentacion y anti-use-after-free

## Formato de Salida Esperado
Entregar siempre:
1. Explicacion breve del diseno elegido y supuestos
2. Cambios en codigo por archivo, en orden:
- `utils/events.h`
- `core/server.h`
- `core/server.c`
- `main.c`
3. Nota de validacion (que se pudo verificar y que falta verificar)

## Prompt de Ejemplo
- "Implementa el motor en mi proyecto C con io_uring y pthreads, sin malloc/free en fast path, siguiendo Master/Readers/Writer."
- "Adapta `core/server.c` y `core/server.h` a una arquitectura con arenas por hilo y pool de contextos en `utils/events.h`."
- "Genera el flujo de arranque en `main.c` y shutdown limpio con senales para el servidor basado en io_uring."
