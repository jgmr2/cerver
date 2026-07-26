// router.js - router minimo basado en location.hash. Sin dependencia
// externa a proposito (mismo criterio del resto del proyecto: nada que
// no haga falta) -- alcanza con las 2 rutas de esta landing (about/wiki).
import { writable } from 'svelte/store'

function currentPath() {
  return window.location.hash.slice(1) || '/about'
}

export const route = writable(currentPath())

window.addEventListener('hashchange', () => {
  route.set(currentPath())
})

export function navigate(path) {
  window.location.hash = path
}
