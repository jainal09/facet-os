# Build and runtime are split so the shipped image contains a single static
# binary and nothing else — no shell, no package manager, no Go toolchain.

FROM golang:1.25.13-alpine AS build
WORKDIR /src

# Dependencies first: this layer only rebuilds when go.mod/go.sum change.
COPY go.mod go.sum ./
RUN go mod download
RUN go install golang.org/x/vuln/cmd/govulncheck@v1.7.0

COPY *.go ./
# The provisioning and DAYS pages are //go:embed-ed into the binary, so they
# have to be in the build context. Without this the build fails at compile time
# rather than shipping an image that 404s.
COPY static/ ./static/

# The tests assert the album-art format as well as the authenticated countdown
# contract. Running them here means a regression fails the image build rather
# than silently shipping data the cube cannot consume.
RUN go vet ./... && go test ./... && govulncheck ./...

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
