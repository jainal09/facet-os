# Build and runtime are split so the shipped image contains a single static
# binary and nothing else — no shell, no package manager, no Go toolchain.

FROM golang:1.25-alpine AS build
WORKDIR /src

# Dependencies first: this layer only rebuilds when go.mod/go.sum change.
COPY go.mod go.sum ./
RUN go mod download

COPY *.go ./

# The tests assert that the album-art output is baseline JPEG, which is the one
# property the firmware cannot work without. Running them here means a
# regression fails the image build rather than silently shipping a cover the
# cube renders as nothing.
RUN go vet ./... && go test ./...

RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /facet-broker .

# Staged so the final image gets a /cache owned by the unprivileged user. A
# fresh named volume inherits ownership from the image, and distroless has no
# shell to mkdir with.
RUN mkdir -p /out/cache

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /facet-broker /facet-broker
# ca-certificates come with distroless/static and are required: the broker makes
# HTTPS calls to accounts.spotify.com and i.scdn.co.
COPY --from=build --chown=nonroot:nonroot /out/cache /cache

USER nonroot:nonroot
EXPOSE 8080
ENTRYPOINT ["/facet-broker"]
CMD ["-addr", ":8080", "-cache-dir", "/cache"]
