# nginx/Dockerfile - build del frontend (frontend/) + nginx sirviendolo
# junto con el reverse proxy hacia el backend (ver nginx/nginx.conf).
#
# Mismo espiritu multi-stage que el Dockerfile de la raiz: el contexto
# de build es la raiz del repo (ver docker-compose.yml, "context: ."),
# no esta carpeta, para poder referenciar frontend/ y nginx/nginx.conf
# desde el mismo build.

## ETAPA 1: BUILD DEL FRONTEND (Vite + Svelte, ver frontend/README.md)
FROM node:20-alpine AS frontend-build
WORKDIR /app
COPY frontend/package*.json ./
RUN npm ci
COPY frontend/ .
RUN npm run build

## ETAPA 2: NGINX
FROM nginx:1.27-alpine
COPY --from=frontend-build /app/dist /usr/share/nginx/html
COPY nginx/nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80
