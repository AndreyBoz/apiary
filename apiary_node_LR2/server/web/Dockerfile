FROM node:22-alpine AS assets
WORKDIR /app
RUN npm install --no-audit --no-fund --save-exact mqtt@5.10.4
FROM nginx:alpine
COPY --from=assets /app/node_modules/mqtt/dist/mqtt.min.js /usr/share/nginx/html/mqtt.min.js
COPY index.html dashboard.js /usr/share/nginx/html/
