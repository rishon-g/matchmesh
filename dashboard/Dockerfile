# The dashboard: build the Angular app, then serve the static files with nginx.

FROM node:24-alpine AS build
WORKDIR /app
# Dependencies first, so editing the app doesn't re-download them.
COPY package.json package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY . .
RUN npx ng build

FROM nginx:1.28-alpine
COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=build /app/dist/dashboard/browser /usr/share/nginx/html
