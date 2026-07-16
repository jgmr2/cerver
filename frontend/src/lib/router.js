import { writable } from 'svelte/store';

// Router minimo basado en History API (pushState/popstate), NO en hash (#).
// A proposito: con hash-routing el navegador nunca manda un request al
// backend al navegar, y no probaria el fallback SPA que sirve index.html
// para rutas desconocidas. Con pushState, un refresh o una URL directa a
// /films SI dispara un GET /films real contra el server en C.
export const currentPath = writable(window.location.pathname);

window.addEventListener('popstate', () => {
  currentPath.set(window.location.pathname);
});

export function navigate(path) {
  if (path !== window.location.pathname) {
    history.pushState({}, '', path);
    currentPath.set(path);
  }
}

// Intercepta clicks en <a href="/algo"> internos para navegar sin recargar
// la pagina; los externos (target=_blank, http(s) a otro host) siguen normal.
export function interceptLinks(node) {
  function onClick(e) {
    const a = e.target.closest('a');
    if (!a || a.target || a.hasAttribute('download')) return;
    const url = new URL(a.href, window.location.href);
    if (url.origin !== window.location.origin) return;

    e.preventDefault();
    navigate(url.pathname);
  }
  node.addEventListener('click', onClick);
  return { destroy: () => node.removeEventListener('click', onClick) };
}
