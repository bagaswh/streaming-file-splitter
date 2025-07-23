FROM alpine:latest

RUN apk add --no-cache \
    clang \
    musl-dev \
    make

WORKDIR /workspace

COPY . .

RUN mkdir -p build

CMD ["make"]