FROM golang:1.26-alpine AS build
WORKDIR /src
COPY server/go.mod server/go.sum ./
RUN go mod download
COPY server/ ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /streetlessd ./cmd/streetlessd

FROM scratch
COPY --from=build /streetlessd /streetlessd
USER 10001:10001
WORKDIR /data
ENV STREETLESS_DB=/data/streetless.sqlite
ENTRYPOINT ["/streetlessd"]
