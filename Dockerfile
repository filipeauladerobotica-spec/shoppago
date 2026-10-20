FROM node:22-alpine AS frontend
WORKDIR /src
COPY package.json package-lock.json ./
RUN npm ci
COPY index.html App.jsx app.css vite.config.js ./
COPY src ./src
RUN npm run build

FROM alpine:3.21 AS backend
RUN apk add --no-cache build-base sqlite-dev json-c-dev openssl-dev curl-dev
WORKDIR /src
COPY server.c .
RUN cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic server.c -o /vitrine \
    -lsqlite3 -ljson-c -lcrypto -lcurl

FROM alpine:3.21
RUN apk add --no-cache sqlite-libs json-c libcrypto3 libcurl ca-certificates \
    && adduser -D -u 10001 app \
    && mkdir /data \
    && chown app:app /data
WORKDIR /app
COPY --from=backend /vitrine /usr/local/bin/vitrine
COPY --from=frontend /src/dist/ ./
EXPOSE 8080
VOLUME ["/data"]
USER app
ENTRYPOINT ["/usr/local/bin/vitrine"]
