FROM node:20-bookworm AS build
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ENV HTTP_PROXY=${HTTP_PROXY}
ENV HTTPS_PROXY=${HTTPS_PROXY}
ENV NO_PROXY=${NO_PROXY}
WORKDIR /src
COPY ui/package.json ui/package-lock.json* ui/ ./
RUN npm ci
RUN npm run build

FROM nginx:1.27-alpine
COPY --from=build /src/dist/ /usr/share/nginx/html/
EXPOSE 80
