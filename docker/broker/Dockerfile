FROM golang:1.24 AS build

ARG TARGETOS=linux
ARG TARGETARCH=amd64

ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ARG GOPROXY=direct
ARG GOSUMDB=sum.golang.org

ENV HTTP_PROXY=${HTTP_PROXY}
ENV HTTPS_PROXY=${HTTPS_PROXY}
ENV NO_PROXY=${NO_PROXY}
ENV GOPROXY=${GOPROXY}
ENV GOSUMDB=${GOSUMDB}

WORKDIR /src
COPY broker/go.mod broker/go.sum ./broker/
WORKDIR /src/broker
RUN go mod download

COPY broker/ /src/broker/

RUN CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} go build -trimpath -ldflags="-s -w" -o /out/agentd-broker ./cmd/agentd-broker
RUN CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} go build -trimpath -ldflags="-s -w" -o /out/agentd-connector ./cmd/agentd-connector
RUN CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} go build -trimpath -ldflags="-s -w" -o /out/agentd-orchestrator ./cmd/agentd-orchestrator
RUN CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} go build -trimpath -ldflags="-s -w" -o /out/agentd-spawn-adapter ./cmd/agentd-spawn-adapter

FROM alpine:3.20
RUN apk add --no-cache ca-certificates
WORKDIR /app
COPY --from=build /out/agentd-broker /usr/local/bin/agentd-broker
COPY --from=build /out/agentd-connector /usr/local/bin/agentd-connector
COPY --from=build /out/agentd-orchestrator /usr/local/bin/agentd-orchestrator
COPY --from=build /out/agentd-spawn-adapter /usr/local/bin/agentd-spawn-adapter
CMD ["/usr/local/bin/agentd-broker"]
